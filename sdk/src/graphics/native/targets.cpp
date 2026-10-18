/**
 * @file        native/targets.cpp
 * @brief       COTT: render targets and resolves of the native renderer (see the header)
 */
#include <rex/graphics/native/targets.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include "shaders/depth_copy_spirv.h"

REXCVAR_DEFINE_INT32(native_debug_skip_copies, 0, "GPU/Native",
                     "Debugging, to measure what the resolve copies cost: 1 skips the depth copies, 2 the "
                     "color copies narrower than the screen (the frame itself is one), 3 both. The image "
                     "is wrong while it is on.");
REXCVAR_DEFINE_BOOL(native_debug_tint_clears, false, "GPU/Native",
                    "Clear color render targets with a color that depends on the resolved strip's "
                    "position instead of the game's clear color (checks the strip mapping)");

namespace rex::graphics::native {

namespace {

// Largest render target height kept (the EDRAM rows after a base can describe far more than the screen).
constexpr uint32_t kMaxRenderTargetHeight = 1280;
// EDRAM tiles: 80x16 samples, 2048 of them.
constexpr uint32_t kEdramTiles = 2048;

uint32_t TileWidthPixels(bool is_64bpp, xenos::MsaaSamples msaa) {
  return (is_64bpp ? 40u : 80u) >> (msaa == xenos::MsaaSamples::k4X ? 1 : 0);
}

uint32_t TileHeightPixels(xenos::MsaaSamples msaa) {
  return 16u >> (msaa >= xenos::MsaaSamples::k2X ? 1 : 0);
}

}  // namespace

NativeTargets::~NativeTargets() {
  Shutdown();
}

bool NativeTargets::Initialize(ui::vulkan::VulkanDevice* device) {
  device_ = device;
  return true;
}

void NativeTargets::Shutdown() {
  if (!device_) {
    return;
  }
  for (auto& [key, image] : targets_) {
    DestroyImage(*image);
  }
  targets_.clear();
  for (Retired& old : retired_) {
    DestroyImage(old.image);
  }
  retired_.clear();
  for (auto& [base, image] : destinations_) {
    DestroyImage(*image);
  }
  destinations_.clear();
  last_color_destination_ = nullptr;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (staging_) dfn.vkDestroyBuffer(device_->device(), staging_, nullptr);
  if (staging_memory_) dfn.vkFreeMemory(device_->device(), staging_memory_, nullptr);
  staging_ = VK_NULL_HANDLE;
  staging_memory_ = VK_NULL_HANDLE;
  staging_size_ = 0;
  if (depth_copy_pipeline_) dfn.vkDestroyPipeline(device_->device(), depth_copy_pipeline_, nullptr);
  if (depth_copy_layout_) dfn.vkDestroyPipelineLayout(device_->device(), depth_copy_layout_, nullptr);
  if (depth_copy_set_layout_) {
    dfn.vkDestroyDescriptorSetLayout(device_->device(), depth_copy_set_layout_, nullptr);
  }
  if (depth_copy_pool_) dfn.vkDestroyDescriptorPool(device_->device(), depth_copy_pool_, nullptr);
  if (depth_copy_sampler_) dfn.vkDestroySampler(device_->device(), depth_copy_sampler_, nullptr);
  depth_copy_pipeline_ = VK_NULL_HANDLE;
  depth_copy_layout_ = VK_NULL_HANDLE;
  depth_copy_set_layout_ = VK_NULL_HANDLE;
  depth_copy_pool_ = VK_NULL_HANDLE;
  depth_copy_sampler_ = VK_NULL_HANDLE;
  device_ = nullptr;
}

bool NativeTargets::CreateImage(NativeImage& image, VkFormat format, VkImageAspectFlags aspect,
                                VkImageUsageFlags usage, uint32_t width, uint32_t height) {
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(device_, info,
                                                        ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                        image.image, image.memory)) {
    REXGPU_ERROR("[native] could not create a {}x{} image (format {})", width, height, int(format));
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(aspect);
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (dfn.vkCreateImageView(device_->device(), &view_info, nullptr, &image.view) != VK_SUCCESS) {
    REXGPU_ERROR("[native] could not create an image view");
    return false;
  }
  image.format = format;
  image.aspect = aspect;
  image.width = width;
  image.height = height;
  image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  image.usage = usage;
  return true;
}

void NativeTargets::DestroyImage(NativeImage& image) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (image.depth_view) dfn.vkDestroyImageView(device_->device(), image.depth_view, nullptr);
  if (image.view) dfn.vkDestroyImageView(device_->device(), image.view, nullptr);
  if (image.image) dfn.vkDestroyImage(device_->device(), image.image, nullptr);
  if (image.memory) dfn.vkFreeMemory(device_->device(), image.memory, nullptr);
  image = NativeImage{};
}

void NativeTargets::Widen(NativeImage& image, uint32_t width) {
  if (before_replace) before_replace();
  NativeImage wide;
  if (!CreateImage(wide, image.format, image.aspect, image.usage, width, image.height)) {
    return;
  }
  REXGPU_INFO("[native] render target widened from {} to {} pixels (untiled pass)", image.width, width);
  wide.msaa = image.msaa;
  wide.last_use = image.last_use;
  // The depth copy's descriptor set pointed at the old view: dropped (its pool is freed at shutdown).
  image.depth_copy_set = VK_NULL_HANDLE;
  retired_.push_back({image, 0});
  image = wide;
}

void NativeTargets::RetireTick() {
  // Three frames in flight at most (kFrameSlots): after more swaps than that, the GPU is past them.
  for (size_t i = 0; i < retired_.size();) {
    if (++retired_[i].age > 8) {
      DestroyImage(retired_[i].image);
      retired_[i] = std::move(retired_.back());
      retired_.pop_back();
    } else {
      ++i;
    }
  }
}

bool NativeTargets::EnsureStagingBuffer(VkDeviceSize size) {
  if (staging_size_ >= size) {
    return true;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (staging_) dfn.vkDestroyBuffer(device_->device(), staging_, nullptr);
  if (staging_memory_) dfn.vkFreeMemory(device_->device(), staging_memory_, nullptr);
  staging_ = VK_NULL_HANDLE;
  staging_memory_ = VK_NULL_HANDLE;
  staging_size_ = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, staging_, staging_memory_)) {
    REXGPU_ERROR("[native] could not create the {} byte resolve staging buffer", size);
    return false;
  }
  staging_size_ = size;
  return true;
}

bool NativeTargets::EnsureDepthCopy() {
  if (depth_copy_pipeline_) return true;
  if (depth_copy_failed_) return false;
  depth_copy_failed_ = true;  // Until everything below is made.
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &depth_copy_sampler_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  binding.pImmutableSamplers = &depth_copy_sampler_;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &depth_copy_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  // One set per depth render target, which live until shutdown (the pool goes with them).
  constexpr uint32_t kMaxSets = 64;
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kMaxSets;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &depth_copy_pool_) != VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(int32_t)};
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &depth_copy_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &depth_copy_layout_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule modules[2] = {};
  const std::pair<const uint32_t*, size_t> code[2] = {
      {shaders::kDepthCopyVs, sizeof(shaders::kDepthCopyVs)},
      {shaders::kDepthCopyPs, sizeof(shaders::kDepthCopyPs)}};
  for (uint32_t i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = code[i].second;
    module_info.pCode = code[i].first;
    if (dfn.vkCreateShaderModule(device, &module_info, nullptr, &modules[i]) != VK_SUCCESS) {
      if (modules[0]) dfn.vkDestroyShaderModule(device, modules[0], nullptr);
      return false;
    }
  }
  VkPipelineShaderStageCreateInfo stages[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  const VkFormat color_format = VK_FORMAT_R32_SFLOAT;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = &rendering;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &rasterization;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = depth_copy_layout_;
  const VkResult result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                        nullptr, &depth_copy_pipeline_);
  for (VkShaderModule module : modules) dfn.vkDestroyShaderModule(device, module, nullptr);
  if (result != VK_SUCCESS) {
    depth_copy_pipeline_ = VK_NULL_HANDLE;
    REXGPU_ERROR("[native] could not create the depth copy pipeline: depth resolves use a buffer");
    return false;
  }
  depth_copy_failed_ = false;
  return true;
}

bool NativeTargets::DepthCopy(VkCommandBuffer cb, NativeImage& src, NativeImage& dest,
                              uint32_t src_x, uint32_t src_y, uint32_t dest_x, uint32_t dest_y,
                              uint32_t width, uint32_t height) {
  if (!EnsureDepthCopy()) return false;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  if (!src.depth_copy_set) {
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = src.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = src.format;
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT);
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &src.depth_view) != VK_SUCCESS) {
      src.depth_view = VK_NULL_HANDLE;
      return false;
    }
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = depth_copy_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &depth_copy_set_layout_;
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &src.depth_copy_set) != VK_SUCCESS) {
      src.depth_copy_set = VK_NULL_HANDLE;
      return false;
    }
    VkDescriptorImageInfo image_info = {};
    image_info.imageView = src.depth_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = src.depth_copy_set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  }
  Transition(cb, src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  Transition(cb, dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo attachment = {};
  attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  attachment.imageView = dest.view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  const VkRect2D area = {{int32_t(dest_x), int32_t(dest_y)}, {width, height}};
  VkRenderingInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = area;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  dfn.vkCmdBeginRendering(cb, &rendering);
  dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_copy_pipeline_);
  dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_copy_layout_, 0, 1,
                              &src.depth_copy_set, 0, nullptr);
  const int32_t offset[2] = {int32_t(src_x) - int32_t(dest_x), int32_t(src_y) - int32_t(dest_y)};
  dfn.vkCmdPushConstants(cb, depth_copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(offset),
                         offset);
  const VkViewport viewport = {float(dest_x), float(dest_y), float(width), float(height), 0.0f, 1.0f};
  dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
  dfn.vkCmdSetScissor(cb, 0, 1, &area);
  dfn.vkCmdDraw(cb, 3, 1, 0, 0);
  dfn.vkCmdEndRendering(cb);
  return true;
}

void NativeTargets::Transition(VkCommandBuffer cb, NativeImage& image, VkImageLayout layout) {
  if (image.layout == layout) {
    return;
  }
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = image.layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(image.aspect);
  device_->functions().vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                            nullptr, 1, &barrier);
  image.layout = layout;
}

void NativeTargets::ReportCopies(uint64_t frames) {
  if (!frames) {
    return;
  }
  const double f = double(frames);
  REXGPU_INFO("[native] copies per frame: {:.1f} color, {:.1f} depth, {:.2f} Mpixels; overwritten "
              "before any draw read them: {:.1f} ({:.2f} Mpixels)",
              color_copies_ / f, depth_copies_ / f, copied_pixels_ / f * 1e-6,
              unread_overwritten_ / f, unread_pixels_ / f * 1e-6);
  color_copies_ = depth_copies_ = unread_overwritten_ = copied_pixels_ = unread_pixels_ = 0;
}

void NativeTargets::Skip(uint32_t reason, const char* text) {
  ++skipped_;
  if (skip_reasons_logged_.insert(reason).second) {
    REXGPU_WARN("[native] resolve skipped: {} (first time; reason {})", text, reason);
  }
}

NativeImage* NativeTargets::GetRenderTarget(const RenderTargetKey& requested) {
  // 4X has no image of its own: it draws into the 1X or 2X image of the same EDRAM tiles. A tile holds
  // 80x16 samples in every mode, so with the same pitch in tiles a 4X pixel (2x2 samples) is two 2X
  // pixels wide, or 2x2 1X pixels (draw.cpp scales). The game clears the start of each surface in 4X and
  // the rest in the surface's own mode, right before drawing in that mode: the image of that EDRAM area
  // used last is the one the clear belongs to. Separate 4X images left most surfaces uncleared (bands
  // at the strip ends in 2X, stale depth hiding parts of the menu's models in 1X).
  RenderTargetKey key = requested;
  if (key.msaa == uint32_t(xenos::MsaaSamples::k4X)) {
    NativeImage* best = nullptr;
    for (auto& [other_key, image] : targets_) {
      if (other_key.base_tiles == key.base_tiles && other_key.pitch_tiles == key.pitch_tiles &&
          other_key.format == key.format && other_key.is_depth == key.is_depth &&
          (!best || image->last_use > best->last_use)) {
        best = image.get();
      }
    }
    if (best) {
      if (min_width > best->width && best->width >= widen_from) Widen(*best, min_width);
      return best;
    }
    key.msaa = uint32_t(xenos::MsaaSamples::k2X);
  }
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    it->second->last_use = ++use_counter_;
    if (min_width > it->second->width && it->second->width >= widen_from) {
      Widen(*it->second, min_width);
    }
    return it->second.get();
  }
  const auto msaa = xenos::MsaaSamples(key.msaa);
  bool is_64bpp = false;
  VkFormat format;
  VkImageAspectFlags aspect;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
  if (key.is_depth) {
    format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    // Not a transfer destination: on the Switch NVK only gives depth images ZCULL (hidden pixels
    // skipped before shading) without TRANSFER_DST. Depth is cleared by rendering, never copied into.
    usage = (usage & ~VK_IMAGE_USAGE_TRANSFER_DST_BIT) | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else {
    switch (xenos::ColorRenderTargetFormat(key.format)) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        format = VK_FORMAT_R8G8B8A8_UNORM;
        break;
      default:
        return nullptr;
    }
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (!key.pitch_tiles || key.base_tiles >= kEdramTiles) {
    return nullptr;
  }
  uint32_t width = key.pitch_tiles * TileWidthPixels(is_64bpp, msaa);
  if (width >= widen_from) width = std::max(width, min_width);
  const uint32_t rows = (kEdramTiles - key.base_tiles) / key.pitch_tiles;
  const uint32_t height = std::min(kMaxRenderTargetHeight, std::max(1u, rows) * TileHeightPixels(msaa));
  auto image = std::make_unique<NativeImage>();
  if (!CreateImage(*image, format, aspect, usage, width, height)) {
    return nullptr;
  }
  REXGPU_INFO("[native] render target {}: base {} pitch {} tiles, format {}, msaa {} -> {}x{}",
              targets_.size() + 1, key.base_tiles, key.pitch_tiles, key.format, key.msaa, width,
              height);
  image->msaa = key.msaa;
  image->last_use = ++use_counter_;
  NativeImage* result = image.get();
  targets_.emplace(key, std::move(image));
  return result;
}

NativeImage* NativeTargets::FindDestination(uint32_t base, uint32_t pitch, uint32_t height,
                                            uint32_t format, uint32_t& strip_x_out) {
  strip_x_out = 0;
  auto exact = destinations_.find(base);
  if (exact != destinations_.end()) {
    return exact->second.get();
  }
  // A later strip of a predicated-tiling resolve: the base moved by the tiled offset of its x position.
  // Only the first row of 32x32 tiles can hold such an offset.
  const uint32_t bytes_per_pixel_log2 = 2;
  for (auto& [dest_base, image] : destinations_) {
    if (image->guest_pitch != pitch || image->guest_height != height ||
        image->guest_format != format || base <= dest_base ||
        base - dest_base >= pitch * xenos::kTextureTileWidthHeight * 4) {
      continue;
    }
    for (uint32_t x = xenos::kTextureTileWidthHeight; x < pitch; x += xenos::kTextureTileWidthHeight) {
      if (dest_base + uint32_t(texture_util::GetTiledOffset2D(int32_t(x), 0, pitch,
                                                              bytes_per_pixel_log2)) == base) {
        strip_x_out = x;
        return image.get();
      }
    }
  }
  return nullptr;
}

NativeImage* NativeTargets::GetColorTarget(const RegisterFile& regs, uint32_t index) {
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto color = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[index]);
  const uint32_t is_64bpp = uint32_t(xenos::IsColorRenderTargetFormat64bpp(color.color_format));
  const uint32_t pitch_tiles =
      xenos::GetSurfacePitchTiles(surface.surface_pitch, surface.msaa_samples, false) << is_64bpp;
  return GetRenderTarget({uint32_t(color.color_base), pitch_tiles, uint32_t(color.color_format),
                          uint32_t(surface.msaa_samples), 0});
}

NativeImage* NativeTargets::GetDepthTarget(const RegisterFile& regs) {
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto depth = regs.Get<reg::RB_DEPTH_INFO>();
  const uint32_t pitch_tiles =
      xenos::GetSurfacePitchTiles(surface.surface_pitch, surface.msaa_samples, false);
  return GetRenderTarget({uint32_t(depth.depth_base), pitch_tiles, uint32_t(depth.depth_format),
                          uint32_t(surface.msaa_samples), 1});
}

std::vector<NativeImage*> NativeTargets::Aliases(NativeImage* image, uint32_t base_tiles,
                                                bool is_depth) {
  std::vector<NativeImage*> result{image};
  for (auto& [key, other] : targets_) {
    if (other.get() != image && key.base_tiles == base_tiles && bool(key.is_depth) == is_depth) {
      result.push_back(other.get());
    }
  }
  return result;
}

std::vector<NativeImage*> NativeTargets::ColorImagesAt(uint32_t base_tiles, uint32_t width) {
  std::vector<NativeImage*> result;
  for (auto& [key, image] : targets_) {
    if (!key.is_depth && key.base_tiles == base_tiles && image->width == width &&
        image->format == VK_FORMAT_R8G8B8A8_UNORM) {
      result.push_back(image.get());
    }
  }
  return result;
}

NativeImage* NativeTargets::FindTexture(uint32_t base) {
  auto it = destinations_.find(base);
  return it != destinations_.end() ? it->second.get() : nullptr;
}

void NativeTargets::Resolve(VkCommandBuffer cb, const RegisterFile& regs,
                            const memory::Memory& memory, uint32_t full_width) {
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, memory, 1, 1, false, false, info)) {
    Skip(1, "GetResolveInfo failed");
    return;
  }
  uint32_t width = info.coordinate_info.width_div_8 * 8;
  const uint32_t height = info.height_div_8 * 8;
  if (full_width > width && info.coordinate_info.edram_offset_x_div_8 == 0 && width) {
    width = full_width;  // The strip's rectangle, for the whole screen drawn at once.
  }
  if (!width || !height) {
    return;  // Clipped away (other strips' resolves during a strip pass): nothing to copy or clear.
  }
  ++resolves_;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();

  // Where the rectangle is inside a render target that starts at `original_base`.
  auto locate = [&](const draw_util::ResolveEdramInfo& edram, uint32_t original_base, uint32_t& x,
                    uint32_t& y) {
    const auto msaa = xenos::MsaaSamples(edram.msaa_samples);
    const uint32_t delta = edram.base_tiles >= original_base ? edram.base_tiles - original_base : 0;
    const uint32_t pitch = std::max(1u, uint32_t(edram.pitch_tiles));
    x = (delta % pitch) * TileWidthPixels(edram.format_is_64bpp, msaa) +
        info.coordinate_info.edram_offset_x_div_8 * 8;
    y = (delta / pitch) * TileHeightPixels(msaa) + info.coordinate_info.edram_offset_y_div_8 * 8;
  };
  auto target_key = [](const draw_util::ResolveEdramInfo& edram, uint32_t original_base) {
    return RenderTargetKey{original_base, edram.pitch_tiles, edram.format, uint32_t(edram.msaa_samples),
                           edram.is_depth};
  };

  // 1. The copy.
  const bool copy_depth = info.IsCopyingDepth();
  if (gpu_mark_resolve) {
    const bool has_copy = info.rb_copy_control.copy_command != xenos::CopyCommand::kNull &&
                          info.copy_dest_extent_length;
    gpu_mark_resolve(width, height,
                     (copy_depth ? 1u : 0u) | (has_copy ? 2u : 0u) | (info.IsClearingColor() ? 4u : 0u) |
                         (info.IsClearingDepth() ? 8u : 0u) |
                         (uint32_t(copy_depth ? info.depth_edram_info.msaa_samples
                                              : info.color_edram_info.msaa_samples) << 8));
  } else if (gpu_mark) {
    gpu_mark(1);
  }
  const draw_util::ResolveEdramInfo& src_edram =
      copy_depth ? info.depth_edram_info : info.color_edram_info;
  const uint32_t src_original = copy_depth ? info.depth_original_base : info.color_original_base;
  uint32_t strip_x = 0;
  if (info.rb_copy_control.copy_command != xenos::CopyCommand::kNull &&
      info.copy_dest_extent_length) {
    NativeImage* src = GetRenderTarget(target_key(src_edram, src_original));
    if (xenos::MsaaSamples(src_edram.msaa_samples) == xenos::MsaaSamples::k4X) {
      src = nullptr;  // Would need a 2:1 horizontal downscale; not seen in the game so far.
      Skip(6, "resolve from a 4X surface");
    }
    const auto dest_format = info.copy_dest_info.copy_dest_format;
    const uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
    const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    VkFormat dest_vk_format = VK_FORMAT_UNDEFINED;
    if (!copy_depth && dest_format == xenos::ColorFormat::k_8_8_8_8) {
      dest_vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (copy_depth && (uint32_t(dest_format) == uint32_t(xenos::TextureFormat::k_24_8) ||
                              uint32_t(dest_format) == uint32_t(xenos::TextureFormat::k_24_8_FLOAT))) {
      // Depth resolves name a texture format (k_24_8, k_24_8_FLOAT), which ColorFormat does not list.
      dest_vk_format = VK_FORMAT_R32_SFLOAT;
    }
    if (!src) {
      if (xenos::MsaaSamples(src_edram.msaa_samples) != xenos::MsaaSamples::k4X) {
        Skip(2, "source render target format not supported yet");
      }
    } else if (dest_vk_format == VK_FORMAT_UNDEFINED) {
      Skip(3, "destination format not supported yet");
    } else if (!dest_pitch.copy_dest_pitch || !dest_pitch.copy_dest_height) {
      Skip(4, "destination with no size");
    } else {
      NativeImage* dest = FindDestination(dest_base, dest_pitch.copy_dest_pitch,
                                          dest_pitch.copy_dest_height, uint32_t(dest_format), strip_x);
      if (!dest) {
        auto image = std::make_unique<NativeImage>();
        if (CreateImage(*image, dest_vk_format, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            (copy_depth ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u),
                        dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height)) {
          image->guest_base = dest_base;
          image->guest_pitch = dest_pitch.copy_dest_pitch;
          image->guest_height = dest_pitch.copy_dest_height;
          image->guest_format = uint32_t(dest_format);
          image->swap_red_blue = !copy_depth && info.copy_dest_info.copy_dest_swap;
          if (copy_depth && xenos::DepthRenderTargetFormat(src_edram.format) ==
                                xenos::DepthRenderTargetFormat::kD24FS8) {
            image->sample_scale = 2.0f;
          }
          REXGPU_INFO("[native] resolve destination {}: {:08X} {}x{} format {}, from EDRAM base {} "
                      "({}, msaa {}, red/blue swap {})",
                      destinations_.size() + 1, dest_base, image->width, image->height,
                      uint32_t(dest_format), src_original, copy_depth ? "depth" : "color",
                      uint32_t(src_edram.msaa_samples),
                      uint32_t(info.copy_dest_info.copy_dest_swap));
          dest = image.get();
          destinations_.emplace(dest_base, std::move(image));
        }
      }
      uint32_t src_x, src_y;
      locate(src_edram, src_original, src_x, src_y);
      const uint32_t dest_x = strip_x + src_x;
      const uint32_t dest_y = src_y;
      if (dest && src_x < src->width && src_y < src->height && dest_x < dest->width &&
          dest_y < dest->height) {
        const uint32_t w = std::min({width, src->width - src_x, dest->width - dest_x});
        const uint32_t h = std::min({height, src->height - src_y, dest->height - dest_y});
        ++(copy_depth ? depth_copies_ : color_copies_);
        copied_pixels_ += uint64_t(w) * h;
        uint32_t overwritten = 0;
        auto& unread = dest->unread;
        for (size_t i = 0; i < unread.size();) {
          const VkRect2D& r = unread[i];
          if (uint32_t(r.offset.x) >= dest_x && uint32_t(r.offset.y) >= dest_y &&
              r.offset.x + r.extent.width <= dest_x + w && r.offset.y + r.extent.height <= dest_y + h) {
            unread_pixels_ += uint64_t(r.extent.width) * r.extent.height;
            ++overwritten;
            unread[i] = unread.back();
            unread.pop_back();
          } else {
            ++i;
          }
        }
        unread_overwritten_ += overwritten;
        if (unread.size() < 64) {
          unread.push_back({{int32_t(dest_x), int32_t(dest_y)}, {w, h}});
        }
        if (trace_copies) {
          REXGPU_INFO("[native copy] after draw {}: {} EDRAM {} ({},{}) -> {:08X} ({},{}) {}x{}, "
                      "covers {} unread",
                      trace_draw, copy_depth ? "depth" : "color", src_original, src_x, src_y, dest_base,
                      dest_x, dest_y, w, h, overwritten);
        }
        if ((REXCVAR_GET(native_debug_skip_copies) & (copy_depth ? 1 : 2)) && (copy_depth || w < 1280)) {
          // Measuring only (native_debug_skip_copies).
        } else {
        if (copy_depth && DepthCopy(cb, *src, *dest, src_x, src_y, dest_x, dest_y, w, h)) {
          // Done by the draw.
        } else {
        Transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cb, *dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (!copy_depth) {
          VkImageCopy region = {};
          region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.srcOffset = {int32_t(src_x), int32_t(src_y), 0};
          region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.dstOffset = {int32_t(dest_x), int32_t(dest_y), 0};
          region.extent = {w, h, 1};
          dfn.vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest->image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
          last_color_destination_ = dest;
        } else if (EnsureStagingBuffer(VkDeviceSize(w) * h * 4)) {
          VkBufferImageCopy to_buffer = {};
          to_buffer.bufferRowLength = w;
          to_buffer.bufferImageHeight = h;
          to_buffer.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          to_buffer.imageOffset = {int32_t(src_x), int32_t(src_y), 0};
          to_buffer.imageExtent = {w, h, 1};
          dfn.vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_,
                                     1, &to_buffer);
          VkBufferMemoryBarrier buffer_barrier = {};
          buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
          buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          buffer_barrier.buffer = staging_;
          buffer_barrier.size = VK_WHOLE_SIZE;
          dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
          VkBufferImageCopy to_image = to_buffer;
          to_image.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          to_image.imageOffset = {int32_t(dest_x), int32_t(dest_y), 0};
          dfn.vkCmdCopyBufferToImage(cb, staging_, dest->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     1, &to_image);
          // The next depth copy reuses the buffer.
          buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
        }
        }
        }
      } else if (dest) {
        Skip(5, "rectangle outside the render target or the destination");
      }
    }
  }

  if (gpu_mark) gpu_mark(2);
  // 2. The clears, of the resolve rectangle only, as Xenos does. Clearing whole images (and every other
  // image at the same EDRAM base) cost about 2 ms of GPU time per resolve on the Switch.
  auto clear_rect = [&](NativeImage& image, const draw_util::ResolveEdramInfo& edram,
                        uint32_t original_base, const VkClearValue& value, bool depth) {
    uint32_t x, y;
    locate(edram, original_base, x, y);
    if (x >= image.width || y >= image.height) {
      return;
    }
    const VkRect2D area = {{int32_t(x), int32_t(y)},
                           {std::min(width, image.width - x), std::min(height, image.height - y)}};
    Transition(cb, image,
               depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = image.view;
    attachment.imageLayout = image.layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue = value;
    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = area;
    rendering.layerCount = 1;
    if (depth) {
      rendering.pDepthAttachment = &attachment;
      rendering.pStencilAttachment = &attachment;
    } else {
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachments = &attachment;
    }
    dfn.vkCmdBeginRendering(cb, &rendering);
    dfn.vkCmdEndRendering(cb);
  };
  if (info.IsClearingColor()) {
    if (NativeImage* rt = GetRenderTarget(target_key(info.color_edram_info, info.color_original_base))) {
      VkClearValue value = {};
      if (REXCVAR_GET(native_debug_tint_clears)) {
        const float t = float(strip_x) / 1280.0f * 6.2831853f;
        value.color.float32[0] = 0.5f + 0.5f * std::cos(t);
        value.color.float32[1] = 0.5f + 0.5f * std::cos(t + 2.094f);
        value.color.float32[2] = 0.5f + 0.5f * std::cos(t + 4.189f);
        value.color.float32[3] = 1.0f;
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          value.color.float32[i] = float((info.rb_color_clear >> (i * 8)) & 0xFF) * (1.0f / 255.0f);
        }
      }
      clear_rect(*rt, info.color_edram_info, info.color_original_base, value, false);
    }
  }
  if (info.IsClearingDepth()) {
    if (NativeImage* rt = GetRenderTarget(target_key(info.depth_edram_info, info.depth_original_base))) {
      VkClearValue value = {};
      const uint32_t depth24 = (info.rb_depth_clear >> 8) & 0xFFFFFF;
      // Host depth keeps the guest range halved, as the emulation and the translated shaders do.
      value.depthStencil.depth = xenos::DepthRenderTargetFormat(info.depth_edram_info.format) ==
                                         xenos::DepthRenderTargetFormat::kD24FS8
                                     ? xenos::Float20e4To32(depth24) * 0.5f
                                     : xenos::UNorm24To32(depth24);
      value.depthStencil.stencil = info.rb_depth_clear & 0xFF;
      clear_rect(*rt, info.depth_edram_info, info.depth_original_base, value, true);
    }
  }
}

void NativeTargets::DumpImages(const std::string& dir, const std::function<VkCommandBuffer()>& begin,
                               const std::function<void()>& submit_and_wait) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  auto dump = [&](NativeImage& image, const std::string& name) {
    const bool depth = image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    const uint32_t width = image.width, height = std::min(image.height, 720u);
    const VkDeviceSize size = VkDeviceSize(width) * height * 4;
    VkBuffer buffer;
    VkDeviceMemory memory;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory)) {
      return;
    }
    VkCommandBuffer cb = begin();
    Transition(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                      : VK_IMAGE_ASPECT_COLOR_BIT),
                             0, 0, 1};
    copy.imageExtent = {width, height, 1};
    dfn.vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &copy);
    submit_and_wait();
    void* data = nullptr;
    if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
      std::ofstream file(dir + "/" + name + ".ppm", std::ios::binary);
      file << "P6\n" << width << " " << height << "\n255\n";
      const auto* bytes = static_cast<const uint8_t*>(data);
      const bool float_texels = depth || image.format == VK_FORMAT_R32_SFLOAT;
      for (uint32_t i = 0; i < width * height; ++i) {
        uint8_t rgb[3];
        if (float_texels) {
          float value;
          std::memcpy(&value, bytes + i * 4, 4);
          const uint8_t v = uint8_t(std::clamp(value * image.sample_scale * (depth ? 2.0f : 1.0f),
                                               0.0f, 1.0f) * 255.0f);
          rgb[0] = rgb[1] = rgb[2] = v;
        } else {
          std::memcpy(rgb, bytes + i * 4, 3);
        }
        file.write(reinterpret_cast<const char*>(rgb), 3);
      }
      dfn.vkUnmapMemory(device, memory);
    }
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
  };
  for (auto& [key, image] : targets_) {
    dump(*image, fmt::format("rt_base{}_pitch{}_fmt{}_msaa{}{}", key.base_tiles, key.pitch_tiles,
                             key.format, key.msaa, key.is_depth ? "_depth" : ""));
  }
  for (auto& [base, image] : destinations_) {
    dump(*image, fmt::format("dest_{:08X}_fmt{}", base, image->guest_format));
  }
  REXGPU_INFO("[native] dumped {} render targets and {} destinations to {}", targets_.size(),
              destinations_.size(), dir);
}

}  // namespace rex::graphics::native
