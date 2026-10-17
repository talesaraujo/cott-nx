/**
 * @file        native/draw.cpp
 * @brief       COTT: draws of the native renderer (Phase 3, milestone M3c)
 *
 * Each guest draw becomes one Vulkan draw with the translated shaders (native/shader_library.h), into the
 * render targets NativeTargets keeps for the EDRAM surfaces the game binds. The fixed-function state is
 * decoded from the registers the way MojoRecomp's Vulkan renderer does it (refs/MojoRecomp,
 * runtime/gpu/vk_presenter.cpp: DecodeViewport, DecodeScissor, UploadShared, GetPipeline,
 * PrepareVertexBindings), because its translated shaders expect exactly that:
 *  - the viewport keeps a positive size and the shader carries the guest's axis signs (g_PosScale);
 *  - with the viewport transform disabled, the shader maps window pixels to clip space;
 *  - host depth is the guest's halved for D24FS8 (the clears and the resolves do the same);
 *  - vertex data is converted from big endian on the CPU, one vertex binding per attribute.
 * MSAA is drawn with one sample, and the strips of predicated tiling land on the same render target
 * through the window offset, as in the EDRAM.
 */
#include <rex/graphics/native/command_processor.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_STRING(native_shader_path, "", "GPU/Native",
                      "Folder with the translated shaders (vs_/ps_<hash>.spv and .meta.json, made by "
                      "tools/shaders)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_shader_capture_path, "", "GPU/Native",
                      "Folder where the microcode of shaders missing from native_shader_path is saved "
                      "(vs_/ps_<hash>.ucode, the translator's input)");
REXCVAR_DEFINE_INT32(native_upload_mb, 48, "GPU/Native",
                     "Upload memory per frame in flight (constants, vertices, textures), in MB")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::native {

namespace {

constexpr uint32_t kTextureSlots = 16;
constexpr uint32_t kDescriptorSetsPerSlot = 2048;
constexpr VkDeviceSize kSharedBytes = 1024;

// Offsets in the shared constants (MojoRecomp vk_presenter.cpp, kShared*).
constexpr uint32_t kSharedBooleans = 256;
constexpr uint32_t kSharedSwappedTexcoords = 260;
constexpr uint32_t kSharedHalfPixelOffset = 264;
constexpr uint32_t kSharedAlphaThreshold = 272;
constexpr uint32_t kSharedTextureSampleScales = 288;
constexpr uint32_t kSharedPosScale = 352;
constexpr uint32_t kSharedPosOffset = 360;
constexpr uint32_t kSharedPosDepthTransform = 368;
constexpr uint32_t kSharedDepthOutputScale = 376;
constexpr uint32_t kSharedLoopConstants = 384;
constexpr uint32_t kSharedBoolFile = 512;
constexpr uint32_t kSharedAspectScale = 544;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

float F32(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int32_t SignExtend15(uint32_t value) {
  value &= 0x7FFF;
  return (value & 0x4000) ? int32_t(value | 0xFFFF8000u) : int32_t(value);
}

VkFormat VertexFormat(const NativeVertexAttribute& a) {
  switch (a.format) {
    case 6:  // k_8_8_8_8
      if (a.is_integer) {
        return a.is_signed ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED;
      }
      return a.is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case 25:  // k_16_16
      if (a.is_integer) {
        return a.is_signed ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED;
      }
      return a.is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    case 26:  // k_16_16_16_16
      if (a.is_integer) {
        return a.is_signed ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED;
      }
      return a.is_signed ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
    case 31:
      return VK_FORMAT_R16G16_SFLOAT;
    case 32:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:
      return a.is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case 36:
      return VK_FORMAT_R32_SFLOAT;
    case 37:
      return VK_FORMAT_R32G32_SFLOAT;
    case 57:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case 38:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

bool IsFloatVertexFormat(uint32_t format) {
  return format == 36 || format == 37 || format == 57 || format == 38;
}

VkCompareOp CompareOp(uint32_t op) {
  static constexpr VkCompareOp ops[] = {
      VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,      VK_COMPARE_OP_EQUAL,
      VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
      VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
  return ops[op & 7];
}

VkStencilOp StencilOp(uint32_t op) {
  static constexpr VkStencilOp ops[] = {
      VK_STENCIL_OP_KEEP,      VK_STENCIL_OP_ZERO,      VK_STENCIL_OP_REPLACE,
      VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return ops[op & 7];
}

VkBlendFactor BlendFactor(uint32_t factor) {
  switch (factor & 0x1F) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;  // reserved encodings, as the emulation does
  }
}

// The alpha equation is scalar on Xenos: color factors mean their alpha counterparts.
VkBlendFactor AlphaBlendFactor(uint32_t factor) {
  switch (factor & 0x1F) {
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return BlendFactor(factor);
  }
}

VkBlendOp BlendOp(uint32_t op) {
  switch (op & 7) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// Depth and stencil state that matters for the EDRAM mode (MojoRecomp NormalizeDepthControl).
uint32_t NormalizeDepthControl(xenos::EdramMode mode, uint32_t depth_control) {
  if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) {
    return 0;
  }
  // Z test without writes and with ALWAYS is no test.
  if ((depth_control & (1u << 1)) && !(depth_control & (1u << 2)) &&
      ((depth_control >> 4) & 7) == 7) {
    depth_control &= ~(1u << 1);
  }
  if (!(depth_control & 1)) {
    depth_control &= 0x7E;
  } else if (!(depth_control & (1u << 7))) {
    depth_control &= 0x000FFFFF;
  }
  return depth_control;
}

// Copies big-endian guest data, swapping each 32-bit word (or 16-bit halves) per the Xenos endian mode.
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t bytes, xenos::Endian endian) {
  size_t i = 0;
  switch (endian) {
    case xenos::Endian::k8in16:
      for (; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case xenos::Endian::k8in32:
      for (; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (; i + 4 <= bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    default:
      break;
  }
  if (i < bytes) {
    std::memcpy(dst + i, src + i, bytes - i);
  }
}

}  // namespace

size_t NativeCommandProcessor::PipelineKeyHash::operator()(const PipelineKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

void NativeCommandProcessor::SkipDraw(uint32_t reason, const char* text) {
  ++draws_skipped_;
  if (draw_skips_logged_.insert(reason).second) {
    REXGPU_WARN("[native] draw skipped: {} (first time; reason {})", text, reason);
  }
}

void NativeCommandProcessor::CaptureMissingShader(const Shader& shader, bool is_pixel) {
  // The microcode in the form tools/shaders/xsh_extract.py writes (guest big-endian dwords), so the
  // translator can add it to the library. In this mode the xenos plugin's shader storage stays closed.
  const std::string dir = REXCVAR_GET(native_shader_capture_path);
  if (dir.empty() || !captured_shaders_.insert(shader.ucode_data_hash()).second) {
    return;
  }
  std::vector<uint32_t> ucode(shader.ucode_dword_count());
  memory::copy_and_swap(ucode.data(), shader.ucode_dwords(), ucode.size());
  const std::string path =
      fmt::format("{}/{}_{:016X}.ucode", dir, is_pixel ? "ps" : "vs", shader.ucode_data_hash());
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(ucode.data(), sizeof(uint32_t), ucode.size(), file);
    std::fclose(file);
    REXGPU_WARN("[native] captured the missing shader into {} (translate it with tools/shaders)", path);
  } else {
    REXGPU_WARN("[native] could not write {}", path);
  }
}

bool NativeCommandProcessor::CreateDrawObjects() {
  ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  const VkDevice vk_device = device->device();
  const ui::vulkan::VulkanDevice::Properties& properties = device->properties();
  if (!properties.bufferDeviceAddress || !properties.dynamicRendering) {
    REXGPU_ERROR("[native] the GPU lacks buffer device addresses or dynamic rendering (needs "
                 "vulkan_native_shader_features, set by the app with gpu_backend=native)");
    return false;
  }

  if (!shader_library_.Initialize(device, REXCVAR_GET(native_shader_path)) ||
      !upload_.Initialize(device, VkDeviceSize(std::max(8, REXCVAR_GET(native_upload_mb))) << 20) ||
      !textures_.Initialize(device, memory_, &upload_)) {
    return false;
  }

  for (uint32_t set = 0; set < 4; ++set) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = kTextureSlots;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (dfn.vkCreateDescriptorSetLayout(vk_device, &info, nullptr, &set_layouts_[set]) != VK_SUCCESS) {
      return false;
    }
  }
  VkPushConstantRange push_range = {};
  push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  push_range.size = 3 * sizeof(uint64_t);
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 4;
  layout_info.pSetLayouts = set_layouts_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    return false;
  }

  auto create_pool = [&](uint32_t sets, VkDescriptorPool& pool) {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sets * kTextureSlots},
        {VK_DESCRIPTOR_TYPE_SAMPLER, sets * kTextureSlots},
    };
    VkDescriptorPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = sets;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    return dfn.vkCreateDescriptorPool(vk_device, &info, nullptr, &pool) == VK_SUCCESS;
  };
  if (!create_pool(2, static_pool_)) {
    return false;
  }
  for (VkDescriptorPool& pool : descriptor_pools_) {
    if (!create_pool(kDescriptorSetsPerSlot, pool)) {
      return false;
    }
  }
  VkDescriptorSetAllocateInfo allocate_info = {};
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.descriptorPool = static_pool_;
  allocate_info.descriptorSetCount = 2;
  allocate_info.pSetLayouts = &set_layouts_[1];
  if (dfn.vkAllocateDescriptorSets(vk_device, &allocate_info, static_sets_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorImageInfo images[2][kTextureSlots] = {};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
      images[i][slot].imageView = textures_.dummy_view(1 + i);
      images[i][slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = static_sets_[i];
    writes[i].descriptorCount = kTextureSlots;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = images[i];
  }
  dfn.vkUpdateDescriptorSets(vk_device, 2, writes, 0, nullptr);
  return true;
}

void NativeCommandProcessor::DestroyDrawObjects() {
  ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  const VkDevice vk_device = device->device();
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline) dfn.vkDestroyPipeline(vk_device, pipeline, nullptr);
  }
  pipelines_.clear();
  for (VkDescriptorPool& pool : descriptor_pools_) {
    if (pool) dfn.vkDestroyDescriptorPool(vk_device, pool, nullptr);
    pool = VK_NULL_HANDLE;
  }
  if (static_pool_) dfn.vkDestroyDescriptorPool(vk_device, static_pool_, nullptr);
  static_pool_ = VK_NULL_HANDLE;
  if (pipeline_layout_) dfn.vkDestroyPipelineLayout(vk_device, pipeline_layout_, nullptr);
  pipeline_layout_ = VK_NULL_HANDLE;
  for (VkDescriptorSetLayout& layout : set_layouts_) {
    if (layout) dfn.vkDestroyDescriptorSetLayout(vk_device, layout, nullptr);
    layout = VK_NULL_HANDLE;
  }
  textures_.Shutdown();
  upload_.Shutdown();
  shader_library_.Shutdown();
}

void NativeCommandProcessor::BeginSlotDrawState() {
  upload_.BeginSlot(slot_index_);
  vulkan_device()->functions().vkResetDescriptorPool(vulkan_device()->device(),
                                                     descriptor_pools_[slot_index_], 0);
  last_views_.fill(VK_NULL_HANDLE);
  last_samplers_.fill(VK_NULL_HANDLE);
  last_sets_[0] = last_sets_[1] = VK_NULL_HANDLE;
  last_shared_offset_ = VK_WHOLE_SIZE;
  textures_.BeginFrame();
  textures_.InitializeDummies(frame_command_buffer_);
  rendering_ = false;
}

void NativeCommandProcessor::EndRendering() {
  if (rendering_) {
    vulkan_device()->functions().vkCmdEndRendering(frame_command_buffer_);
    rendering_ = false;
  }
}

VkPipeline NativeCommandProcessor::GetPipeline(const PipelineKey& key, const NativeShader& vs,
                                               const NativeShader* ps) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }
  ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();

  std::vector<VkVertexInputBindingDescription> bindings;
  std::vector<VkVertexInputAttributeDescription> attributes;
  for (const NativeVertexAttribute& a : vs.attributes) {
    const VkFormat format = VertexFormat(a);
    if (format == VK_FORMAT_UNDEFINED || !a.stride_dwords) {
      REXGPU_WARN("[native] VS {:016X}: vertex format {} not supported", vs.hash, a.format);
      pipelines_.emplace(key, VK_NULL_HANDLE);
      return VK_NULL_HANDLE;
    }
    const uint32_t binding = uint32_t(bindings.size());
    bindings.push_back({binding, a.stride_dwords * 4, VK_VERTEX_INPUT_RATE_VERTEX});
    attributes.push_back({a.location, binding, format, 0});
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs.module;
  stages[0].pName = "main";
  uint32_t stage_count = 1;
  const uint32_t spec_value = key.alpha_test ? (0x2u | (((key.alpha_test - 1) & 7) << 8)) : 0;
  const VkSpecializationMapEntry spec_entry = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec_info = {};
  spec_info.mapEntryCount = 1;
  spec_info.pMapEntries = &spec_entry;
  spec_info.dataSize = sizeof(spec_value);
  spec_info.pData = &spec_value;
  if (ps) {
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps->module;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &spec_info;
    stage_count = 2;
  }

  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = uint32_t(bindings.size());
  vertex_input.pVertexBindingDescriptions = bindings.data();
  vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
  vertex_input.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.restart ? VK_TRUE : VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  const bool polygonal = key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                         key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                         key.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.frontFace = (key.raster & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.cullMode = VK_CULL_MODE_NONE;
  if (polygonal && !(key.attachments & 8)) {  // bit 3: rectangle list expanded on the CPU, never culled
    if (key.raster & 1) raster.cullMode |= VK_CULL_MODE_FRONT_BIT;
    if (key.raster & 2) raster.cullMode |= VK_CULL_MODE_BACK_BIT;
  }
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  const uint32_t dc = key.depth_control;
  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = (dc >> 1) & 1;
  depth.depthWriteEnable = (dc >> 2) & 1;
  depth.depthCompareOp = CompareOp(dc >> 4);
  depth.stencilTestEnable = dc & 1;
  depth.maxDepthBounds = 1.0f;
  if (depth.stencilTestEnable) {
    depth.front.compareOp = CompareOp(dc >> 8);
    depth.front.failOp = StencilOp(dc >> 11);
    depth.front.passOp = StencilOp(dc >> 14);
    depth.front.depthFailOp = StencilOp(dc >> 17);
    if (polygonal && ((dc >> 7) & 1)) {
      depth.back.compareOp = CompareOp(dc >> 20);
      depth.back.failOp = StencilOp(dc >> 23);
      depth.back.passOp = StencilOp(dc >> 26);
      depth.back.depthFailOp = StencilOp(dc >> 29);
    } else {
      depth.back = depth.front;
    }
  }

  VkPipelineColorBlendAttachmentState blends[2] = {};
  for (uint32_t target = 0; target < 2; ++target) {
    VkPipelineColorBlendAttachmentState& blend = blends[target];
    const uint32_t mask = (key.color_mask >> (target * 4)) & 0xF;
    const uint32_t control = target == 0 ? key.blend0 : key.blend1;
    blend.colorWriteMask = mask;  // R, G, B, A bits match
    blend.srcColorBlendFactor = BlendFactor(control);
    blend.colorBlendOp = BlendOp(control >> 5);
    blend.dstColorBlendFactor = BlendFactor(control >> 8);
    blend.srcAlphaBlendFactor = AlphaBlendFactor(control >> 16);
    blend.alphaBlendOp = BlendOp(control >> 21);
    blend.dstAlphaBlendFactor = AlphaBlendFactor(control >> 24);
    const bool identity = blend.srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
                          blend.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                          blend.colorBlendOp == VK_BLEND_OP_ADD &&
                          blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                          blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
                          blend.alphaBlendOp == VK_BLEND_OP_ADD;
    blend.blendEnable = (mask && !identity) ? VK_TRUE : VK_FALSE;
  }
  VkPipelineColorBlendStateCreateInfo blend_state = {};
  blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend_state.attachmentCount = 2;
  blend_state.pAttachments = blends;

  VkDynamicState dynamic_states[6] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
  uint32_t dynamic_count = 3;
  if (depth.stencilTestEnable) {
    dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    dynamic_states[dynamic_count++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = dynamic_count;
  dynamic.pDynamicStates = dynamic_states;

  const VkFormat color_formats[2] = {(key.attachments & 1) ? kColorFormat : VK_FORMAT_UNDEFINED,
                                     (key.attachments & 2) ? kColorFormat : VK_FORMAT_UNDEFINED};
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = 2;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = (key.attachments & 4) ? kDepthFormat : VK_FORMAT_UNDEFINED;
  rendering.stencilAttachmentFormat = rendering.depthAttachmentFormat;

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend_state;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      dfn.vkCreateGraphicsPipelines(device->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    REXGPU_ERROR("[native] pipeline VS {:016X} PS {:016X} failed ({})", key.vs, key.ps, int(result));
    pipeline = VK_NULL_HANDLE;
  } else {
    REXGPU_DEBUG("[native] pipeline {}: VS {:016X} PS {:016X} topology {} mask {:02X} depth {:08X}",
                 pipelines_.size() + 1, key.vs, key.ps, key.topology, key.color_mask,
                 key.depth_control);
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool NativeCommandProcessor::Draw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                  const IndexBufferInfo* index_buffer_info) {
  const RegisterFile& regs = *register_file_;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device()->functions();
  const xenos::EdramMode mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) {
    SkipDraw(1, "EDRAM mode other than color/depth");
    return true;
  }
  if (!index_count) {
    return true;
  }

  // 1. Shaders.
  Shader* guest_vs = active_vertex_shader();
  Shader* guest_ps = active_pixel_shader();
  if (!guest_vs) {
    SkipDraw(2, "no vertex shader");
    return true;
  }
  NativeShader* vs = shader_library_.Get(guest_vs->ucode_data_hash(), false);
  const bool color_pass = mode == xenos::EdramMode::kColorDepth && guest_ps;
  NativeShader* ps = color_pass ? shader_library_.Get(guest_ps->ucode_data_hash(), true) : nullptr;
  if (!vs || (color_pass && !ps)) {
    if (!vs) CaptureMissingShader(*guest_vs, false);
    if (color_pass && !ps) CaptureMissingShader(*guest_ps, true);
    SkipDraw(3, "translated shader missing (see the warning above)");
    return true;
  }

  // 2. Primitive.
  VkPrimitiveTopology topology;
  bool rectangles = false, quads = false;
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleFan: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      rectangles = true;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    default:
      SkipDraw(100 + uint32_t(prim_type), "primitive type not supported yet");
      return true;
  }

  // 3. Render targets and state.
  const uint32_t full_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  uint32_t color_mask = 0;
  if (color_pass) {
    if (ps->color_output_mask & 1) color_mask |= full_color_mask & 0xF;
    if (ps->color_output_mask & 2) color_mask |= full_color_mask & 0xF0;
  }
  const uint32_t depth_control = NormalizeDepthControl(mode, regs[XE_GPU_REG_RB_DEPTHCONTROL]);
  const bool uses_depth = (depth_control & 7) != 0;
  NativeImage* targets[3] = {};
  if (color_mask & 0xF) targets[0] = targets_.GetColorTarget(regs, 0);
  if (color_mask & 0xF0) targets[1] = targets_.GetColorTarget(regs, 1);
  if (uses_depth) targets[2] = targets_.GetDepthTarget(regs);
  if ((color_mask & 0xF) && !targets[0]) {
    SkipDraw(4, "color render target format not supported yet");
    return true;
  }
  if (!targets[0]) color_mask &= ~0xFu;
  if (!targets[1]) color_mask &= ~0xF0u;
  if (!targets[0] && !targets[1] && !targets[2]) {
    return true;  // Writes nothing.
  }
  VkExtent2D target_extent = {UINT32_MAX, UINT32_MAX};
  for (NativeImage* target : targets) {
    if (target) {
      target_extent.width = std::min(target_extent.width, target->width);
      target_extent.height = std::min(target_extent.height, target->height);
    }
  }
  // 4X draws go to the 1X or 2X image of the same EDRAM area (NativeTargets::GetRenderTarget): a 4X
  // pixel is 2 image pixels wide, and 2 high in a 1X image. Everything below is in guest pixels until
  // the viewport and the scissor are scaled at the end.
  uint32_t scale_x = 1, scale_y = 1;
  if (regs.Get<reg::RB_SURFACE_INFO>().msaa_samples == xenos::MsaaSamples::k4X) {
    scale_x = 2;
    for (NativeImage* target : targets) {
      if (target && target->msaa == uint32_t(xenos::MsaaSamples::k1X)) scale_y = 2;
    }
  }
  const VkExtent2D extent = {target_extent.width / scale_x, target_extent.height / scale_y};

  // Scissor (MojoRecomp DecodeScissor): window scissor moved by the window offset, then the screen one.
  const uint32_t window_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  const uint32_t window_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  int32_t sx0 = int32_t(window_tl & 0x3FFF), sy0 = int32_t((window_tl >> 16) & 0x3FFF);
  int32_t sx1 = int32_t(window_br & 0x3FFF), sy1 = int32_t((window_br >> 16) & 0x3FFF);
  if (!(window_tl & 0x80000000u)) {
    const uint32_t offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    sx0 += SignExtend15(offset);
    sx1 += SignExtend15(offset);
    sy0 += SignExtend15(offset >> 16);
    sy1 += SignExtend15(offset >> 16);
  }
  const uint32_t screen_tl = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL];
  const uint32_t screen_br = regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR];
  sx0 = std::clamp(std::max(sx0, int32_t(screen_tl & 0x7FFF)), 0, int32_t(extent.width));
  sy0 = std::clamp(std::max(sy0, int32_t((screen_tl >> 16) & 0x7FFF)), 0, int32_t(extent.height));
  sx1 = std::clamp(std::min(sx1, int32_t(screen_br & 0x7FFF)), 0, int32_t(extent.width));
  sy1 = std::clamp(std::min(sy1, int32_t((screen_br >> 16) & 0x7FFF)), 0, int32_t(extent.height));
  if (sx1 <= sx0 || sy1 <= sy0) {
    if (trace_frame_) {
      REXGPU_INFO("[native trace] draw {}: VS {:016X} PS {:016X} clipped (window {:08X}-{:08X} "
                  "offset {:08X})",
                  trace_draw_index_++, vs->hash, ps ? ps->hash : 0, window_tl, window_br,
                  regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
    }
    return true;  // Fully clipped (another strip's part of the screen).
  }
  const VkRect2D scissor = {{sx0 * int32_t(scale_x), sy0 * int32_t(scale_y)},
                            {uint32_t(sx1 - sx0) * scale_x, uint32_t(sy1 - sy0) * scale_y}};

  // Window offset applied to vertices (PA_SU_SC_MODE_CNTL::vtx_window_offset_enable).
  int32_t window_x = 0, window_y = 0;
  const uint32_t su_sc_mode = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  if (su_sc_mode & (1u << 16)) {
    const uint32_t offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    window_x = SignExtend15(offset);
    window_y = SignExtend15(offset >> 16);
  }

  // Viewport (MojoRecomp DecodeViewport; its pixel shaders all export depth, so the depth range is the
  // full one and the shader's g_PosDepthTransform does the rest).
  const uint32_t vte = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  VkViewport viewport = {};
  if (vte & 1) {
    const float xs = F32(regs[XE_GPU_REG_PA_CL_VPORT_XSCALE]);
    const float ys = (vte & 4) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    const float xo = (vte & 2) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    const float yo = (vte & 8) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    viewport.x = xo - std::fabs(xs) + float(window_x);
    viewport.y = yo - std::fabs(ys) + float(window_y);
    viewport.width = 2.0f * std::fabs(xs);
    viewport.height = 2.0f * std::fabs(ys);
  } else {
    viewport.width = float(extent.width);
    viewport.height = float(extent.height);
  }
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  const bool depth_float24 = (regs[XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 1;
  if ((depth_control & 2) && depth_float24) {
    viewport.maxDepth = 0.5f;
  }
  viewport.x *= float(scale_x);
  viewport.width *= float(scale_x);
  viewport.y *= float(scale_y);
  viewport.height *= float(scale_y);
  if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) ||
      !std::isfinite(viewport.height) || viewport.width <= 0.0f || viewport.height <= 0.0f) {
    return true;  // Degenerate: no coverage.
  }

  uint32_t alpha_test = 0;
  const uint32_t color_control = regs[XE_GPU_REG_RB_COLORCONTROL];
  if (color_pass && (color_control & 8) && (color_control & 7) != 7) {
    alpha_test = (color_control & 7) + 1;
  }
  const bool restart = index_buffer_info &&
                       (prim_type == xenos::PrimitiveType::kLineStrip ||
                        prim_type == xenos::PrimitiveType::kTriangleFan ||
                        prim_type == xenos::PrimitiveType::kTriangleStrip) &&
                       (regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL] & (1u << 21));

  PipelineKey key;
  key.vs = vs->hash;
  key.ps = ps ? ps->hash : 0;
  key.topology = uint32_t(topology);
  key.color_mask = color_mask;
  key.blend0 = (color_mask & 0xF) ? regs[XE_GPU_REG_RB_BLENDCONTROL0] : 0;
  key.blend1 = (color_mask & 0xF0) ? regs[XE_GPU_REG_RB_BLENDCONTROL1] : 0;
  key.depth_control = targets[2] ? depth_control : 0;
  key.raster = su_sc_mode & 7;
  key.alpha_test = alpha_test;
  key.attachments = (targets[0] ? 1 : 0) | (targets[1] ? 2 : 0) | (targets[2] ? 4 : 0) |
                    (rectangles ? 8 : 0);
  key.restart = restart ? 1 : 0;
  const VkPipeline pipeline = GetPipeline(key, *vs, ps);
  if (!pipeline) {
    SkipDraw(5, "pipeline could not be created");
    return true;
  }

  // 4. Indices: the vertex each element uses after the Xenos offset and clamp, as 32-bit host indices
  // relative to the lowest vertex uploaded.
  const uint32_t index_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  const uint32_t min_vertex = regs[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
  const uint32_t max_vertex = regs[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
  uint32_t clamped_indices = 0, raw_lo = UINT32_MAX, raw_hi = 0;
  auto remap = [&](uint32_t index) {
    uint32_t v = (index + index_offset) & 0xFFFFFF;
    raw_lo = std::min(raw_lo, v);
    raw_hi = std::max(raw_hi, v);
    if (min_vertex <= max_vertex && (v < min_vertex || v > max_vertex)) {
      v = std::clamp(v, min_vertex, max_vertex);
      ++clamped_indices;
    }
    return v;
  };
  constexpr uint32_t kRestart = UINT32_MAX;
  std::vector<uint32_t>& indices = draw_indices_;
  indices.resize(index_count);
  const uint8_t* physical = memory_->TranslatePhysical(0);
  if (index_buffer_info) {
    const bool index32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const uint32_t restart_index =
        regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & (index32 ? 0xFFFFFFFFu : 0xFFFFu);
    uint32_t endian_xor = 0;
    switch (index_buffer_info->endianness) {
      case xenos::Endian::k8in16: endian_xor = 1; break;
      case xenos::Endian::k8in32: endian_xor = 3; break;
      case xenos::Endian::k16in32: endian_xor = 2; break;
      default: break;
    }
    const uint32_t base = index_buffer_info->guest_base;
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t raw;
      if (index32) {
        const uint32_t a = base + i * 4;
        raw = uint32_t(physical[((a + 0) ^ endian_xor) & 0x1FFFFFFF]) |
              (uint32_t(physical[((a + 1) ^ endian_xor) & 0x1FFFFFFF]) << 8) |
              (uint32_t(physical[((a + 2) ^ endian_xor) & 0x1FFFFFFF]) << 16) |
              (uint32_t(physical[((a + 3) ^ endian_xor) & 0x1FFFFFFF]) << 24);
      } else {
        const uint32_t a = base + i * 2;
        raw = uint32_t(physical[((a + 0) ^ endian_xor) & 0x1FFFFFFF]) |
              (uint32_t(physical[((a + 1) ^ endian_xor) & 0x1FFFFFFF]) << 8);
      }
      indices[i] = (restart && raw == restart_index) ? kRestart : remap(raw);
    }
  } else {
    for (uint32_t i = 0; i < index_count; ++i) indices[i] = remap(i);
  }
  uint32_t lo = UINT32_MAX, hi = 0;
  for (uint32_t index : indices) {
    if (index != kRestart) {
      lo = std::min(lo, index);
      hi = std::max(hi, index);
    }
  }
  if (lo > hi) {
    return true;
  }

  if (trace_frame_) {
    REXGPU_INFO("[native trace]   indices {} ({}-bit, endian {}): {}..{}, clamp {}..{} changed {}, "
                "offset {}",
                index_count, index_buffer_info && index_buffer_info->format ==
                                     xenos::IndexFormat::kInt32 ? 32 : 16,
                index_buffer_info ? uint32_t(index_buffer_info->endianness) : 0, raw_lo, raw_hi,
                min_vertex, max_vertex, clamped_indices, index_offset);
    for (const NativeVertexAttribute& a : vs->attributes) {
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(a.fetch_slot);
      REXGPU_INFO("[native trace]   attribute {}: fetch {} at {:08X} size {} dwords endian {}, "
                  "format {} stride {} offset {}",
                  a.location, a.fetch_slot, fetch.address << 2, uint32_t(fetch.size),
                  uint32_t(fetch.endian), a.format, a.stride_dwords, a.offset_dwords);
    }
  }

  // Rectangles: three corners each; the fourth is rebuilt on the CPU (below) from the diagonal, found as
  // the longest edge, and each rectangle becomes two triangles over four new vertices.
  const uint32_t rectangle_count = rectangles ? index_count / 3 : 0;
  std::vector<uint32_t>& host_indices = draw_host_indices_;
  host_indices.clear();
  std::vector<uint32_t> rectangle_order;  // per rectangle: other, diagonal A, diagonal B (guest vertices)
  // Bounds and depth of the rectangles, for the EDRAM alias of depth clears (step 10).
  float rect_min[2] = {FLT_MAX, FLT_MAX}, rect_max[2] = {-FLT_MAX, -FLT_MAX};
  float rect_z = NAN;
  if (rectangles) {
    const NativeVertexAttribute* position = nullptr;
    for (const NativeVertexAttribute& a : vs->attributes) {
      if (a.location == 0 && (a.format == 37 || a.format == 57 || a.format == 38)) {
        position = &a;
        break;
      }
    }
    xenos::xe_gpu_vertex_fetch_t position_fetch = {};
    if (position) position_fetch = regs.GetVertexFetch(position->fetch_slot);
    for (uint32_t r = 0; r < rectangle_count; ++r) {
      const uint32_t* corner = &indices[r * 3];
      uint32_t other = 2, diagonal_a = 0, diagonal_b = 1;
      if (position) {
        float xy[3][2];
        const uint32_t position_dwords = position->format == 37 ? 2 : 3;
        for (uint32_t c = 0; c < 3; ++c) {
          uint32_t words[3] = {};
          const uint32_t address = (position_fetch.address << 2) +
                                   corner[c] * position->stride_dwords * 4 +
                                   position->offset_dwords * 4;
          CopySwapped(reinterpret_cast<uint8_t*>(words), physical + (address & 0x1FFFFFFF),
                      position_dwords * 4, position_fetch.endian);
          xy[c][0] = F32(words[0]);
          xy[c][1] = F32(words[1]);
          for (uint32_t i = 0; i < 2; ++i) {
            rect_min[i] = std::min(rect_min[i], xy[c][i]);
            rect_max[i] = std::max(rect_max[i], xy[c][i]);
          }
          if (position_dwords == 3) rect_z = F32(words[2]);
        }
        auto distance = [&](uint32_t a, uint32_t b) {
          const float dx = xy[a][0] - xy[b][0], dy = xy[a][1] - xy[b][1];
          return dx * dx + dy * dy;
        };
        const float d01 = distance(0, 1), d12 = distance(1, 2), d20 = distance(2, 0);
        if (d12 >= d01 && d12 >= d20) {
          other = 0, diagonal_a = 1, diagonal_b = 2;
        } else if (d20 >= d01) {
          other = 1, diagonal_a = 2, diagonal_b = 0;
        }
      }
      // Keep other, A, B a cyclic permutation of 0, 1, 2 (same winding as the guest's corners).
      if ((other + 1) % 3 != diagonal_a) std::swap(diagonal_a, diagonal_b);
      rectangle_order.push_back(corner[other]);
      rectangle_order.push_back(corner[diagonal_a]);
      rectangle_order.push_back(corner[diagonal_b]);
      const uint32_t v = r * 4;
      for (uint32_t i : {v, v + 1, v + 2, v + 2, v + 1, v + 3}) host_indices.push_back(i);
    }
  } else if (quads) {
    for (uint32_t q = 0; q + 4 <= index_count; q += 4) {
      for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) host_indices.push_back(indices[q + i] - lo);
    }
  } else {
    host_indices.reserve(index_count);
    for (uint32_t index : indices) host_indices.push_back(index == kRestart ? kRestart : index - lo);
  }
  if (host_indices.empty()) {
    return true;
  }

  // 5. Command buffer, textures (may record uploads, so before the rendering pass begins).
  VkCommandBuffer cb = EnsureFrame();
  std::array<TextureBinding, kTextureSlots> bindings;
  float sample_scales[kTextureSlots];
  std::fill(std::begin(sample_scales), std::end(sample_scales), 1.0f);
  bool any_texture = false;
  for (const NativeShader* shader : {vs, ps}) {
    if (!shader) continue;
    for (uint32_t slot : shader->texture_slots) {
      if (slot >= kTextureSlots) continue;
      const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
      textures_.Prepare(cb, fetch, targets_, [this]() { EndRendering(); GpuMark(3); },
                        bindings[slot]);
      if (trace_frame_) {
        REXGPU_INFO("[native trace]   {} slot {}: {:08X} format {} {}x{} swizzle {:03X} tiled {} "
                    "endian {} mips {} {}",
                    shader == ps ? "PS" : "VS", slot, fetch.base_address << 12,
                    uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                    uint32_t(fetch.swizzle), uint32_t(fetch.tiled), uint32_t(fetch.endianness),
                    uint32_t(fetch.mip_max_level),
                    targets_.FindTexture(fetch.base_address << 12) ? "resolved" : "memory");
      }
      if (shader == ps && traced_shaders_.insert(ps->hash ^ slot).second) {
        NativeImage* resolved = targets_.FindTexture(fetch.base_address << 12);
        REXGPU_DEBUG("[native] PS {:016X} slot {}: {:08X} format {} {}x{} swizzle {:03X} tiled {} "
                    "endian {} -> {}",
                    ps->hash, slot, fetch.base_address << 12, uint32_t(fetch.format),
                    fetch.size_2d.width + 1, fetch.size_2d.height + 1, uint32_t(fetch.swizzle),
                    uint32_t(fetch.tiled), uint32_t(fetch.endianness),
                    resolved ? "resolve destination" : "memory texture");
      }
      sample_scales[slot] = bindings[slot].sample_scale;
      any_texture = true;
    }
  }
  for (TextureBinding& binding : bindings) {
    if (!binding.view) {
      binding.view = textures_.dummy_view(0);
      binding.sampler = textures_.default_sampler();
    }
  }

  // 6. Upload memory: constants and vertices.
  // Only the float constants each shader can read (most read a few dozen of the 256).
  VkDeviceSize vs_constants_offset, ps_constants_offset, shared_offset;
  const VkDeviceSize vs_constants_bytes = std::max(1u, vs->constant_count) * 16;
  const VkDeviceSize ps_constants_bytes = std::max(1u, ps ? ps->constant_count : 1u) * 16;
  uint8_t* vs_constants = upload_.Allocate(vs_constants_bytes, 256, vs_constants_offset);
  uint8_t* ps_constants = upload_.Allocate(ps_constants_bytes, 256, ps_constants_offset);
  if (!vs_constants || !ps_constants) {
    SkipDraw(6, "upload memory full for this frame (raise native_upload_mb)");
    return true;
  }
  std::memcpy(vs_constants, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], vs_constants_bytes);
  std::memcpy(ps_constants, &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], ps_constants_bytes);
  static_assert(sizeof(shared_scratch_) == kSharedBytes);
  uint8_t* shared = shared_scratch_.data();

  // Shared constants (MojoRecomp UploadShared).
  std::memset(shared, 0, kSharedBytes);
  for (uint32_t bank = 0; bank < 4; ++bank) {
    for (uint32_t slot = 0; slot < kTextureSlots; ++slot) {
      std::memcpy(shared + bank * 64 + slot * 4, &slot, 4);
    }
  }
  std::memcpy(shared + kSharedBoolFile, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 32);
  std::memcpy(shared + kSharedLoopConstants, &regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 128);
  uint32_t bool_fold = 0;
  for (uint32_t i = 0; i < 8; ++i) bool_fold |= regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i];
  std::memcpy(shared + kSharedBooleans, &bool_fold, 4);
  const uint32_t swapped_texcoords = 0;
  std::memcpy(shared + kSharedSwappedTexcoords, &swapped_texcoords, 4);
  float pos_scale[2] = {1.0f, 1.0f}, pos_offset[2] = {0.0f, 0.0f}, half_pixel[2] = {0.0f, 0.0f};
  const bool pixel_center_zero = !(regs[XE_GPU_REG_PA_SU_VTX_CNTL] & 1);
  if (!(vte & 1)) {
    const float w = float(extent.width), h = float(extent.height);
    pos_scale[0] = 2.0f / w;
    pos_scale[1] = 2.0f / h;
    pos_offset[0] = -1.0f + float(window_x) * pos_scale[0];
    pos_offset[1] = -1.0f + float(window_y) * pos_scale[1];
    if (pixel_center_zero) {
      half_pixel[0] = 1.0f / w;
      half_pixel[1] = 1.0f / h;
    }
  } else {
    const float xs = F32(regs[XE_GPU_REG_PA_CL_VPORT_XSCALE]);
    const float ys = (vte & 4) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    if (std::isfinite(xs) && xs != 0.0f) pos_scale[0] = std::signbit(xs) ? -1.0f : 1.0f;
    if (std::isfinite(ys) && ys != 0.0f) pos_scale[1] = std::signbit(ys) ? -1.0f : 1.0f;
    if (pixel_center_zero) {
      if (std::isfinite(xs) && xs != 0.0f) half_pixel[0] = 0.5f / std::fabs(xs);
      if (std::isfinite(ys) && ys != 0.0f) half_pixel[1] = 0.5f / std::fabs(ys);
    }
  }
  std::memcpy(shared + kSharedHalfPixelOffset, half_pixel, 8);
  std::memcpy(shared + kSharedPosScale, pos_scale, 8);
  std::memcpy(shared + kSharedPosOffset, pos_offset, 8);
  {
    const uint32_t clip_control = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
    const bool clipping_disabled = clip_control & (1u << 16);
    const bool dx_clip_space = clip_control & (1u << 19);
    const float z_scale = (vte & (1u << 4)) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float z_offset = (vte & (1u << 5)) ? F32(regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    float depth_transform[2] = {1.0f, 0.0f};
    if (clipping_disabled) {
      if (std::isfinite(z_scale) && std::isfinite(z_offset)) {
        depth_transform[0] = z_scale;
        depth_transform[1] = z_offset;
      }
    } else if (!dx_clip_space) {
      depth_transform[0] = 0.5f;
      depth_transform[1] = 0.5f;
    }
    if (!clipping_disabled && std::isfinite(z_scale) && std::isfinite(z_offset)) {
      const float host_offset = dx_clip_space ? z_offset : z_offset - z_scale;
      const float host_scale = dx_clip_space ? z_scale : z_scale * 2.0f;
      if (std::clamp(host_offset, 0.0f, 1.0f) > std::clamp(host_offset + host_scale, 0.0f, 1.0f)) {
        // Reversed depth range: reflect NDC Z, the host range stays increasing.
        depth_transform[0] = -depth_transform[0];
        depth_transform[1] = 1.0f - depth_transform[1];
      }
    }
    std::memcpy(shared + kSharedPosDepthTransform, depth_transform, 8);
  }
  const float depth_output_scale = depth_float24 ? 0.5f : 1.0f;
  std::memcpy(shared + kSharedDepthOutputScale, &depth_output_scale, 4);
  const float aspect_scale[2] = {1.0f, 1.0f};
  std::memcpy(shared + kSharedAspectScale, aspect_scale, 8);
  const float alpha_threshold = F32(regs[XE_GPU_REG_RB_ALPHA_REF]);
  std::memcpy(shared + kSharedAlphaThreshold, &alpha_threshold, 4);
  std::memcpy(shared + kSharedTextureSampleScales, sample_scales, sizeof(sample_scales));
  if (last_shared_offset_ != VK_WHOLE_SIZE &&
      !std::memcmp(shared_scratch_.data(), last_shared_.data(), kSharedBytes)) {
    shared_offset = last_shared_offset_;
  } else {
    uint8_t* out = upload_.Allocate(kSharedBytes, 256, shared_offset);
    if (!out) {
      SkipDraw(6, "upload memory full for this frame (raise native_upload_mb)");
      return true;
    }
    std::memcpy(out, shared_scratch_.data(), kSharedBytes);
    last_shared_ = shared_scratch_;
    last_shared_offset_ = shared_offset;
  }

  // Vertices: each fetch the shader reads, from the lowest to the highest vertex used, swapped to little
  // endian once per fetch constant and stride.
  std::array<VkBuffer, 32> vertex_buffers;
  std::array<VkDeviceSize, 32> vertex_offsets;
  uint32_t binding_count = 0;
  struct Uploaded {
    uint32_t slot, stride;
    VkDeviceSize offset;
  };
  Uploaded uploaded[16];
  uint32_t uploaded_count = 0;
  const uint32_t vertex_count = rectangles ? rectangle_count * 4 : hi - lo + 1;
  for (const NativeVertexAttribute& a : vs->attributes) {
    if (binding_count >= vertex_buffers.size()) break;
    VkDeviceSize offset = VK_WHOLE_SIZE;
    for (uint32_t i = 0; i < uploaded_count; ++i) {
      if (uploaded[i].slot == a.fetch_slot && uploaded[i].stride == a.stride_dwords) {
        offset = uploaded[i].offset;
      }
    }
    if (offset == VK_WHOLE_SIZE) {
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(a.fetch_slot);
      const uint32_t stride = a.stride_dwords * 4;
      const uint32_t address = fetch.address << 2;
      const uint64_t fetch_bytes = uint64_t(fetch.size) * 4;
      if (fetch.type != xenos::FetchConstantType::kVertex ||
          uint64_t(hi + 1) * stride > fetch_bytes + stride) {
        SkipDraw(7, "vertex fetch outside its buffer");
        return true;
      }
      uint8_t* out = upload_.Allocate(VkDeviceSize(vertex_count) * stride, 16, offset);
      if (!out) {
        SkipDraw(6, "upload memory full for this frame (raise native_upload_mb)");
        return true;
      }
      if (!rectangles) {
        CopySwapped(out, physical + ((address + lo * stride) & 0x1FFFFFFF),
                    size_t(vertex_count) * stride, fetch.endian);
      } else {
        // Which dwords of the vertex hold floats (the fourth corner is A + B - other for those, a copy
        // of the other corner for the rest).
        std::vector<bool> is_float(a.stride_dwords, false);
        for (const NativeVertexAttribute& b : vs->attributes) {
          if (b.fetch_slot != a.fetch_slot || !IsFloatVertexFormat(b.format)) continue;
          const uint32_t dwords = b.format == 36 ? 1 : b.format == 37 ? 2 : b.format == 57 ? 3 : 4;
          for (uint32_t d = 0; d < dwords && b.offset_dwords + d < a.stride_dwords; ++d) {
            is_float[b.offset_dwords + d] = true;
          }
        }
        for (uint32_t r = 0; r < rectangle_count; ++r) {
          uint8_t* v = out + size_t(r) * 4 * stride;
          for (uint32_t c = 0; c < 3; ++c) {
            CopySwapped(v + c * stride,
                        physical + ((address + rectangle_order[r * 3 + c] * stride) & 0x1FFFFFFF),
                        stride, fetch.endian);
          }
          for (uint32_t d = 0; d < a.stride_dwords; ++d) {
            float other, diagonal_a, diagonal_b;
            std::memcpy(&other, v + d * 4, 4);
            std::memcpy(&diagonal_a, v + stride + d * 4, 4);
            std::memcpy(&diagonal_b, v + 2 * stride + d * 4, 4);
            const float fourth = is_float[d] ? diagonal_a + diagonal_b - other : other;
            std::memcpy(v + 3 * stride + d * 4, &fourth, 4);
          }
        }
      }
      if (uploaded_count < 16) {
        uploaded[uploaded_count++] = {a.fetch_slot, a.stride_dwords, offset};
      }
    }
    vertex_buffers[binding_count] = upload_.buffer();
    vertex_offsets[binding_count] = offset + VkDeviceSize(a.offset_dwords) * 4;
    ++binding_count;
  }
  VkDeviceSize index_offset_bytes;
  uint8_t* index_out =
      upload_.Allocate(VkDeviceSize(host_indices.size()) * 4, 4, index_offset_bytes);
  if (!index_out) {
    SkipDraw(6, "upload memory full for this frame (raise native_upload_mb)");
    return true;
  }
  std::memcpy(index_out, host_indices.data(), host_indices.size() * 4);

  // 7. Descriptor sets for the textures (reused while the bindings do not change).
  VkDescriptorSet sets[4] = {last_sets_[0], static_sets_[0], static_sets_[1], last_sets_[1]};
  bool same = last_sets_[0] != VK_NULL_HANDLE;
  for (uint32_t i = 0; same && i < kTextureSlots; ++i) {
    same = last_views_[i] == bindings[i].view && last_samplers_[i] == bindings[i].sampler;
  }
  if (!same) {
    const VkDescriptorSetLayout layouts[2] = {set_layouts_[0], set_layouts_[3]};
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pools_[slot_index_];
    allocate_info.descriptorSetCount = 2;
    allocate_info.pSetLayouts = layouts;
    VkDescriptorSet new_sets[2];
    if (dfn.vkAllocateDescriptorSets(vulkan_device()->device(), &allocate_info, new_sets) !=
        VK_SUCCESS) {
      SkipDraw(8, "descriptor sets exhausted for this frame");
      return true;
    }
    VkDescriptorImageInfo images[kTextureSlots] = {}, samplers[kTextureSlots] = {};
    for (uint32_t i = 0; i < kTextureSlots; ++i) {
      images[i].imageView = bindings[i].view;
      images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      samplers[i].sampler = bindings[i].sampler;
      last_views_[i] = bindings[i].view;
      last_samplers_[i] = bindings[i].sampler;
    }
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = new_sets[i];
      writes[i].descriptorCount = kTextureSlots;
      writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[i].pImageInfo = i ? samplers : images;
    }
    dfn.vkUpdateDescriptorSets(vulkan_device()->device(), 2, writes, 0, nullptr);
    last_sets_[0] = sets[0] = new_sets[0];
    last_sets_[1] = sets[3] = new_sets[1];
  }
  (void)any_texture;

  GpuMark(0);

  // 8. The rendering pass: the same attachments keep it open across draws.
  if (rendering_ && (rendering_targets_[0] != targets[0] || rendering_targets_[1] != targets[1] ||
                     rendering_targets_[2] != targets[2])) {
    EndRendering();
  }
  if (!rendering_) {
    for (uint32_t i = 0; i < 3; ++i) {
      if (targets[i]) {
        targets_.Transition(cb, *targets[i],
                            i < 2 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      }
    }
    VkRenderingAttachmentInfo colors[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      colors[i].imageView = targets[i] ? targets[i]->view : VK_NULL_HANDLE;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_stencil.imageView = targets[2] ? targets[2]->view : VK_NULL_HANDLE;
    depth_stencil.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = {{0, 0}, target_extent};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 2;
    rendering_info.pColorAttachments = colors;
    rendering_info.pDepthAttachment = targets[2] ? &depth_stencil : nullptr;
    rendering_info.pStencilAttachment = targets[2] ? &depth_stencil : nullptr;
    dfn.vkCmdBeginRendering(cb, &rendering_info);
    rendering_ = true;
    for (uint32_t i = 0; i < 3; ++i) rendering_targets_[i] = targets[i];
    rendering_extent_ = target_extent;
  }

  if (trace_frame_) {
    float first[4] = {};
    for (const NativeVertexAttribute& a : vs->attributes) {
      if (a.location == 0 && IsFloatVertexFormat(a.format)) {
        const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(a.fetch_slot);
        uint32_t words[4] = {};
        const uint32_t dwords = std::min(4u, a.stride_dwords - a.offset_dwords);
        CopySwapped(reinterpret_cast<uint8_t*>(words),
                    physical + (((fetch.address << 2) + lo * a.stride_dwords * 4 +
                                 a.offset_dwords * 4) & 0x1FFFFFFF),
                    dwords * 4, fetch.endian);
        for (uint32_t i = 0; i < dwords; ++i) first[i] = F32(words[i]);
        break;
      }
    }
    REXGPU_INFO("[native trace] draw {}: VS {:016X} PS {:016X} prim {} count {} targets {}/{}/{} "
                "mask {:02X} depth {:08X} vte {:03X} win ({},{}) offs_en {} viewport {:.0f},{:.0f} "
                "{:.0f}x{:.0f} scissor {},{} {}x{} extent {}x{} v0 ({:.1f},{:.1f},{:.2f},{:.2f}) "
                "blend {:08X} alpha test {} ref {:.3f} cull {:X}",
                trace_draw_index_++, vs->hash, ps ? ps->hash : 0, uint32_t(prim_type), index_count,
                targets[0] ? fmt::format("C0@{}", regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) : "-",
                targets[1] ? fmt::format("C1@{}", regs[XE_GPU_REG_RB_COLOR1_INFO] & 0xFFF) : "-",
                targets[2] ? fmt::format("D@{}", regs[XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF) : "-",
                color_mask, key.depth_control, vte, window_x, window_y, (su_sc_mode >> 16) & 1,
                viewport.x, viewport.y, viewport.width, viewport.height, scissor.offset.x,
                scissor.offset.y, scissor.extent.width, scissor.extent.height, extent.width,
                extent.height, first[0], first[1], first[2], first[3], key.blend0, alpha_test,
                F32(regs[XE_GPU_REG_RB_ALPHA_REF]), su_sc_mode & 7);
  }

  // 9. The draw.
  GpuMarkDraw(vs->hash, ps ? ps->hash : 0, scissor.extent.width, scissor.extent.height);
  dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4, sets, 0,
                              nullptr);
  dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
  dfn.vkCmdSetScissor(cb, 0, 1, &scissor);
  float blend_constants[4];
  for (uint32_t i = 0; i < 4; ++i) blend_constants[i] = F32(regs[XE_GPU_REG_RB_BLEND_RED + i]);
  dfn.vkCmdSetBlendConstants(cb, blend_constants);
  if (key.depth_control & 1) {
    const uint32_t front = regs[XE_GPU_REG_RB_STENCILREFMASK];
    const bool polygonal = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                           topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    const uint32_t back = (polygonal && ((key.depth_control >> 7) & 1))
                              ? regs[XE_GPU_REG_RB_STENCILREFMASK_BF]
                              : front;
    auto set_face = [&](VkStencilFaceFlags face, uint32_t value) {
      dfn.vkCmdSetStencilReference(cb, face, value & 0xFF);
      dfn.vkCmdSetStencilCompareMask(cb, face, (value >> 8) & 0xFF);
      dfn.vkCmdSetStencilWriteMask(cb, face, (value >> 16) & 0xFF);
    };
    set_face(VK_STENCIL_FACE_FRONT_BIT, front);
    set_face(VK_STENCIL_FACE_BACK_BIT, back);
  }
  const uint64_t push[3] = {upload_.address() + vs_constants_offset,
                            upload_.address() + ps_constants_offset,
                            upload_.address() + shared_offset};
  dfn.vkCmdPushConstants(cb, pipeline_layout_,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                         push);
  if (binding_count) {
    dfn.vkCmdBindVertexBuffers(cb, 0, binding_count, vertex_buffers.data(), vertex_offsets.data());
  }
  dfn.vkCmdBindIndexBuffer(cb, upload_.buffer(), index_offset_bytes, VK_INDEX_TYPE_UINT32);
  dfn.vkCmdDrawIndexed(cb, uint32_t(host_indices.size()), 1, 0, 0, 0);
  ++draws_done_;

  // 10. EDRAM aliasing of depth clears. Color and depth share the EDRAM, and the game clears some color
  // surfaces (the motion blur's velocity, color 1 at the scene's end) by drawing a depth-only rectangle
  // over them, in 4X, with ALWAYS: the color surface then holds the depth and stencil bits. Here each
  // surface is its own image, so the color images at that base get the same bits over the rectangle.
  if (rectangles && targets[2] && !targets[0] && !targets[1] && std::isfinite(rect_z) &&
      !(vte & 1) && (key.depth_control & 6) == 6 && ((key.depth_control >> 4) & 7) == 7) {
    const int32_t x0 = std::clamp(int32_t(std::lround(rect_min[0])) + window_x, sx0, sx1);
    const int32_t x1 = std::clamp(int32_t(std::lround(rect_max[0])) + window_x, sx0, sx1);
    const int32_t y0 = std::clamp(int32_t(std::lround(rect_min[1])) + window_y, sy0, sy1);
    const int32_t y1 = std::clamp(int32_t(std::lround(rect_max[1])) + window_y, sy0, sy1);
    const uint32_t depth_base = regs[XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF;
    std::vector<NativeImage*> aliases = targets_.ColorImagesAt(depth_base, targets[2]->width);
    if (x1 > x0 && y1 > y0 && !aliases.empty()) {
      const float z = std::clamp(rect_z, 0.0f, 1.0f);
      const uint32_t depth24 = depth_float24 ? xenos::Float32To20e4(z, true)
                                             : uint32_t(z * 16777215.0f + 0.5f);
      // ZPASS = REPLACE writes the reference; otherwise assume the cleared stencil, 0.
      const uint32_t stencil = ((key.depth_control & 1) && ((key.depth_control >> 14) & 7) == 2)
                                   ? regs[XE_GPU_REG_RB_STENCILREFMASK] & 0xFF
                                   : 0;
      const uint32_t bits = (depth24 << 8) | stencil;
      VkClearValue value = {};
      for (uint32_t i = 0; i < 4; ++i) {
        value.color.float32[i] = float((bits >> (i * 8)) & 0xFF) * (1.0f / 255.0f);
      }
      const VkRect2D area = {{x0 * int32_t(scale_x), y0 * int32_t(scale_y)},
                             {uint32_t(x1 - x0) * scale_x, uint32_t(y1 - y0) * scale_y}};
      EndRendering();
      for (NativeImage* color : aliases) {
        targets_.Transition(cb, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment = {};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = color->view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue = value;
        VkRenderingInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea = area;
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &attachment;
        dfn.vkCmdBeginRendering(cb, &info);
        dfn.vkCmdEndRendering(cb);
      }
      if (trace_frame_) {
        REXGPU_INFO("[native trace]   depth clear aliased onto {} color image(s) at base {}: "
                    "{},{} {}x{} bits {:08X}",
                    aliases.size(), depth_base, area.offset.x, area.offset.y, area.extent.width,
                    area.extent.height, bits);
      }
    }
  }
  return true;
}

}  // namespace rex::graphics::native
