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
  return true;
}

void NativeTargets::DestroyImage(NativeImage& image) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (image.view) dfn.vkDestroyImageView(device_->device(), image.view, nullptr);
  if (image.image) dfn.vkDestroyImage(device_->device(), image.image, nullptr);
  if (image.memory) dfn.vkFreeMemory(device_->device(), image.memory, nullptr);
  image = NativeImage{};
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

void NativeTargets::Skip(uint32_t reason, const char* text) {
  ++skipped_;
  if (skip_reasons_logged_.insert(reason).second) {
    REXGPU_WARN("[native] resolve skipped: {} (first time; reason {})", text, reason);
  }
}

NativeImage* NativeTargets::GetRenderTarget(const RenderTargetKey& requested) {
  // 4X and 2X share one image: their pixels have the same height (two sample rows) and a 4X pixel is
  // two 2X pixels wide, so 4X draws are scaled horizontally by 2 (see draw.cpp). The game clears the
  // start of each strip in 4X and the rest in 2X before drawing the scene in 2X: separate images left
  // most of the scene uncleared (bands at the strip ends).
  RenderTargetKey key = requested;
  if (key.msaa == uint32_t(xenos::MsaaSamples::k4X)) {
    key.msaa = uint32_t(xenos::MsaaSamples::k2X);
  }
  auto it = targets_.find(key);
  if (it != targets_.end()) {
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
    usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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
  const uint32_t width = key.pitch_tiles * TileWidthPixels(is_64bpp, msaa);
  const uint32_t rows = (kEdramTiles - key.base_tiles) / key.pitch_tiles;
  const uint32_t height = std::min(kMaxRenderTargetHeight, std::max(1u, rows) * TileHeightPixels(msaa));
  auto image = std::make_unique<NativeImage>();
  if (!CreateImage(*image, format, aspect, usage, width, height)) {
    return nullptr;
  }
  REXGPU_INFO("[native] render target {}: base {} pitch {} tiles, format {}, msaa {} -> {}x{}",
              targets_.size() + 1, key.base_tiles, key.pitch_tiles, key.format, key.msaa, width,
              height);
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

NativeImage* NativeTargets::FindTexture(uint32_t base) {
  auto it = destinations_.find(base);
  return it != destinations_.end() ? it->second.get() : nullptr;
}

void NativeTargets::Resolve(VkCommandBuffer cb, const RegisterFile& regs,
                            const memory::Memory& memory) {
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, memory, 1, 1, false, false, info)) {
    Skip(1, "GetResolveInfo failed");
    return;
  }
  const uint32_t width = info.coordinate_info.width_div_8 * 8;
  const uint32_t height = info.height_div_8 * 8;
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
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
      } else if (dest) {
        Skip(5, "rectangle outside the render target or the destination");
      }
    }
  }

  // 2. The clears, of the whole image for now (the rectangle covers the used part in every resolve seen).
  if (info.IsClearingColor()) {
    if (NativeImage* rt = GetRenderTarget(target_key(info.color_edram_info, info.color_original_base))) {
      VkClearColorValue color = {};
      if (REXCVAR_GET(native_debug_tint_clears)) {
        const float t = float(strip_x) / 1280.0f * 6.2831853f;
        color.float32[0] = 0.5f + 0.5f * std::cos(t);
        color.float32[1] = 0.5f + 0.5f * std::cos(t + 2.094f);
        color.float32[2] = 0.5f + 0.5f * std::cos(t + 4.189f);
        color.float32[3] = 1.0f;
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          color.float32[i] = float((info.rb_color_clear >> (i * 8)) & 0xFF) * (1.0f / 255.0f);
        }
      }
      // Every image of the same EDRAM area, whatever its MSAA mode: the game clears in 4X and draws
      // in 2X (the same samples on the console; separate images here).
      const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
      for (NativeImage* alias : Aliases(rt, info.color_original_base, false)) {
        Transition(cb, *alias, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dfn.vkCmdClearColorImage(cb, alias->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                 &range);
      }
    }
  }
  if (info.IsClearingDepth()) {
    if (NativeImage* rt = GetRenderTarget(target_key(info.depth_edram_info, info.depth_original_base))) {
      VkClearDepthStencilValue value = {};
      const uint32_t depth24 = (info.rb_depth_clear >> 8) & 0xFFFFFF;
      // Host depth keeps the guest range halved, as the emulation and the translated shaders do.
      value.depth = xenos::DepthRenderTargetFormat(info.depth_edram_info.format) ==
                            xenos::DepthRenderTargetFormat::kD24FS8
                        ? xenos::Float20e4To32(depth24) * 0.5f
                        : xenos::UNorm24To32(depth24);
      value.stencil = info.rb_depth_clear & 0xFF;
      const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange(
          VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
      for (NativeImage* alias : Aliases(rt, info.depth_original_base, true)) {
        Transition(cb, *alias, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dfn.vkCmdClearDepthStencilImage(cb, alias->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value,
                                        1, &range);
      }
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
