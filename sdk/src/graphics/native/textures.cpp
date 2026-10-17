/**
 * @file        native/textures.cpp
 * @brief       COTT: textures of the native renderer (see the header)
 */
#include <rex/graphics/native/textures.h>

#include <algorithm>
#include <cstring>

#include <xxhash.h>

#include <rex/graphics/native/targets.h>
#include <rex/graphics/native/upload_arena.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

namespace {

struct HostFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t block_size = 1;  // texels per block side (4 for the DXT formats)
  uint32_t bytes_log2 = 0;  // bytes per block
};

HostFormat GetHostFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
      return {VK_FORMAT_R8_UNORM, 1, 0};
    case F::k_8_8:
      return {VK_FORMAT_R8G8_UNORM, 1, 1};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return {VK_FORMAT_R8G8B8A8_UNORM, 1, 2};
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 3};
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 4};
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 4};
    case F::k_16_16_FLOAT:
      return {VK_FORMAT_R16G16_SFLOAT, 1, 2};
    case F::k_16_16_16_16_FLOAT:
      return {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 3};
    case F::k_32_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, 1, 2};
    default:
      return {};
  }
}

// Byte address swap of each Xenos endian mode, applied to every byte address: 8-in-16 swaps the bytes of
// each 16-bit word, 8-in-32 reverses each 32-bit word, 16-in-32 swaps its halves.
uint32_t EndianXor(xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      return 1;
    case xenos::Endian::k8in32:
      return 3;
    case xenos::Endian::k16in32:
      return 2;
    default:
      return 0;
  }
}

VkComponentSwizzle ToVkSwizzle(uint32_t component) {
  switch (component & 7) {
    case 0:
      return VK_COMPONENT_SWIZZLE_R;
    case 1:
      return VK_COMPONENT_SWIZZLE_G;
    case 2:
      return VK_COMPONENT_SWIZZLE_B;
    case 3:
      return VK_COMPONENT_SWIZZLE_A;
    case 4:
      return VK_COMPONENT_SWIZZLE_ZERO;
    default:
      return VK_COMPONENT_SWIZZLE_ONE;
  }
}

VkSamplerAddressMode ToVkAddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

}  // namespace

NativeTextures::~NativeTextures() {
  Shutdown();
}

bool NativeTextures::Initialize(ui::vulkan::VulkanDevice* device, const memory::Memory* memory,
                                UploadArena* upload) {
  device_ = device;
  memory_ = memory;
  upload_ = upload;
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();

  // 1x1 black images for slots the shader does not sample, or whose texture is not supported.
  for (uint32_t dimension = 0; dimension < 3; ++dimension) {
    Dummy& dummy = dummies_[dimension];
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = dimension == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = dimension == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = dimension == 2 ? 6 : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, dummy.image, dummy.memory)) {
      REXGPU_ERROR("[native] could not create the dummy textures");
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = dummy.image;
    view_info.viewType = dimension == 0   ? VK_IMAGE_VIEW_TYPE_2D
                         : dimension == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                          : VK_IMAGE_VIEW_TYPE_CUBE;
    view_info.format = info.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
    if (dfn.vkCreateImageView(device->device(), &view_info, nullptr, &dummy.view) != VK_SUCCESS) {
      return false;
    }
  }
  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return dfn.vkCreateSampler(device->device(), &sampler_info, nullptr, &default_sampler_) ==
         VK_SUCCESS;
}

void NativeTextures::InitializeDummies(VkCommandBuffer cb) {
  if (dummies_initialized_) {
    return;
  }
  dummies_initialized_ = true;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  for (uint32_t dimension = 0; dimension < 3; ++dimension) {
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                           dimension == 2 ? 6u : 1u};
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dummies_[dimension].image;
    barrier.subresourceRange = range;
    dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearColorValue black = {};
    dfn.vkCmdClearColorImage(cb, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
}

void NativeTextures::Shutdown() {
  if (!device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [key, texture] : textures_) {
    for (auto& [swizzle, view] : texture->views) dfn.vkDestroyImageView(device, view, nullptr);
    if (texture->image) dfn.vkDestroyImage(device, texture->image, nullptr);
    if (texture->memory) dfn.vkFreeMemory(device, texture->memory, nullptr);
  }
  textures_.clear();
  for (auto& [image, views] : resolved_views_) {
    for (auto& [swizzle, view] : views) dfn.vkDestroyImageView(device, view, nullptr);
  }
  resolved_views_.clear();
  for (auto& [key, sampler] : samplers_) dfn.vkDestroySampler(device, sampler, nullptr);
  samplers_.clear();
  if (default_sampler_) dfn.vkDestroySampler(device, default_sampler_, nullptr);
  default_sampler_ = VK_NULL_HANDLE;
  for (Dummy& dummy : dummies_) {
    if (dummy.view) dfn.vkDestroyImageView(device, dummy.view, nullptr);
    if (dummy.image) dfn.vkDestroyImage(device, dummy.image, nullptr);
    if (dummy.memory) dfn.vkFreeMemory(device, dummy.memory, nullptr);
    dummy = Dummy{};
  }
  device_ = nullptr;
}

void NativeTextures::Unsupported(uint32_t format, const char* what) {
  if (unsupported_logged_.insert(format).second) {
    REXGPU_WARN("[native] texture format {} not supported yet ({}): sampled as black", format, what);
  }
}

VkImageView NativeTextures::GetView(VkImage image, VkFormat format, uint32_t levels,
                                    uint32_t swizzle,
                                    std::unordered_map<uint32_t, VkImageView>& views) {
  auto it = views.find(swizzle);
  if (it != views.end()) {
    return it->second;
  }
  VkImageViewCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.components = {ToVkSwizzle(swizzle), ToVkSwizzle(swizzle >> 3), ToVkSwizzle(swizzle >> 6),
                     ToVkSwizzle(swizzle >> 9)};
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  device_->functions().vkCreateImageView(device_->device(), &info, nullptr, &view);
  views.emplace(swizzle, view);
  return view;
}

VkSampler NativeTextures::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t levels) {
  const uint32_t mip_min = std::min(uint32_t(fetch.mip_min_level), levels - 1);
  const uint32_t mip_max = std::min(uint32_t(fetch.mip_max_level), levels - 1);
  const uint64_t key = uint64_t(fetch.clamp_x) | (uint64_t(fetch.clamp_y) << 3) |
                       (uint64_t(fetch.mag_filter) << 6) | (uint64_t(fetch.min_filter) << 8) |
                       (uint64_t(fetch.mip_filter) << 10) | (uint64_t(mip_min) << 12) |
                       (uint64_t(mip_max) << 16) | (uint64_t(fetch.aniso_filter) << 20);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  VkSamplerCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = fetch.mag_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST
                                                                    : VK_FILTER_LINEAR;
  info.minFilter = fetch.min_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST
                                                                    : VK_FILTER_LINEAR;
  info.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = ToVkAddressMode(fetch.clamp_x);
  info.addressModeV = ToVkAddressMode(fetch.clamp_y);
  info.addressModeW = ToVkAddressMode(fetch.clamp_z);
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  if (fetch.mip_filter == xenos::TextureFilter::kBaseMap) {
    info.minLod = info.maxLod = float(mip_min);
  } else {
    info.minLod = float(mip_min);
    info.maxLod = float(mip_max);
  }
  if (fetch.aniso_filter != xenos::AnisoFilter::kDisabled &&
      fetch.aniso_filter != xenos::AnisoFilter::kUseFetchConst &&
      device_->properties().samplerAnisotropy) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = std::min(float(1u << (uint32_t(fetch.aniso_filter) - 1)),
                                  device_->properties().maxSamplerAnisotropy);
  }
  VkSampler sampler = VK_NULL_HANDLE;
  device_->functions().vkCreateSampler(device_->device(), &info, nullptr, &sampler);
  samplers_.emplace(key, sampler);
  return sampler;
}

NativeTextures::Texture* NativeTextures::GetTexture(const xenos::xe_gpu_texture_fetch_t& /*fetch*/,
                                                    VkFormat format, const Key& key,
                                                    uint32_t levels) {
  auto it = textures_.find(key);
  if (it != textures_.end()) {
    return it->second.get();
  }
  auto texture = std::make_unique<Texture>();
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {key.width, key.height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          device_, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, texture->image,
          texture->memory)) {
    REXGPU_ERROR("[native] could not create a {}x{} texture (format {})", key.width, key.height,
                 int(format));
    texture.reset();
  } else {
    texture->format = format;
    texture->levels = levels;
  }
  Texture* result = texture.get();
  textures_.emplace(key, std::move(texture));
  return result;
}

bool NativeTextures::Upload(VkCommandBuffer cb, const xenos::xe_gpu_texture_fetch_t& fetch,
                            Texture& texture, uint32_t width, uint32_t height, uint32_t base_page,
                            uint32_t mip_page) {
  const HostFormat host = GetHostFormat(fetch.format);
  const uint32_t bytes = 1u << host.bytes_log2;
  const uint32_t endian_xor = EndianXor(fetch.endianness);
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled, fetch.format,
      fetch.packed_mips, base_page != 0, texture.levels - 1);
  const uint8_t* physical = memory_->TranslatePhysical(0);

  std::vector<VkBufferImageCopy> copies;
  for (uint32_t level = 0; level < texture.levels; ++level) {
    const uint32_t level_width = std::max(width >> level, 1u);
    const uint32_t level_height = std::max(height >> level, 1u);
    const uint32_t blocks_x = (level_width + host.block_size - 1) / host.block_size;
    const uint32_t blocks_y = (level_height + host.block_size - 1) / host.block_size;
    // Where the level is stored: its own place, or the packed mip tail of a smaller level.
    const uint32_t stored = std::min(level, layout.packed_level);
    uint32_t source;
    uint32_t row_pitch_bytes;
    if (stored == 0) {
      source = base_page << 12;
      row_pitch_bytes = layout.base.row_pitch_bytes;
    } else {
      source = (mip_page << 12) + layout.mip_offsets_bytes[stored];
      row_pitch_bytes = layout.mips[stored].row_pitch_bytes;
    }
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, level, offset_x, offset_y,
                                       offset_z);
    }
    const uint32_t pitch_blocks = row_pitch_bytes >> host.bytes_log2;

    VkDeviceSize offset;
    uint8_t* out = upload_->Allocate(VkDeviceSize(blocks_x) * blocks_y * bytes, 16, offset);
    if (!out) {
      return false;
    }
    for (uint32_t y = 0; y < blocks_y; ++y) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const uint32_t block = source + uint32_t(fetch.tiled ? texture_util::GetTiledOffset2D(
                                                                   int32_t(x + offset_x),
                                                                   int32_t(y + offset_y),
                                                                   pitch_blocks, host.bytes_log2)
                                                             : (y + offset_y) * row_pitch_bytes +
                                                                   (x + offset_x) * bytes);
        if (!endian_xor) {
          std::memcpy(out, physical + (block & 0x1FFFFFFF), bytes);
        } else {
          for (uint32_t i = 0; i < bytes; ++i) {
            out[i] = physical[((block + i) ^ endian_xor) & 0x1FFFFFFF];
          }
        }
        out += bytes;
      }
    }
    VkBufferImageCopy copy = {};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    copy.imageExtent = {level_width, level_height, 1};
    copies.push_back(copy);
    upload_bytes_ += VkDeviceSize(blocks_x) * blocks_y * bytes;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = texture.uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, 1};
  dfn.vkCmdPipelineBarrier(cb,
                           texture.uploaded ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  dfn.vkCmdCopyBufferToImage(cb, upload_->buffer(), texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
  texture.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  texture.uploaded = true;
  ++uploads_;
  return true;
}

void NativeTextures::Prepare(VkCommandBuffer cb, const xenos::xe_gpu_texture_fetch_t& fetch,
                             NativeTargets& targets, const std::function<void()>& before_commands,
                             TextureBinding& out) {
  out.view = dummies_[0].view;
  out.sampler = default_sampler_;
  out.sample_scale = 1.0f;
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return;  // Unbound slot behind control flow the shader does not take.
  }
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    Unsupported(1000 + uint32_t(fetch.dimension), "dimension other than 2D");
    return;
  }

  // Render-to-texture: the destination of a resolve at this address.
  if (NativeImage* resolved = targets.FindTexture(fetch.base_address << 12)) {
    if (resolved->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      before_commands();
      targets.Transition(cb, *resolved, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    uint32_t swizzle = fetch.swizzle;
    if (resolved->swap_red_blue) {
      // Components 0 and 2 of the guest texture are the render target's blue and red.
      uint32_t swapped = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t c = (swizzle >> (i * 3)) & 7;
        c = c == 0 ? 2 : c == 2 ? 0 : c;
        swapped |= c << (i * 3);
      }
      swizzle = swapped;
    }
    out.view = GetView(resolved->image, resolved->format, 1, swizzle,
                       resolved_views_[resolved->image]);
    out.sampler = GetSampler(fetch, 1);
    out.sample_scale = resolved->sample_scale;
    return;
  }

  const HostFormat host = GetHostFormat(fetch.format);
  if (host.format == VK_FORMAT_UNDEFINED) {
    Unsupported(uint32_t(fetch.format), "memory texture");
    return;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!base_page) {
    return;
  }
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  const uint32_t levels = mip_page ? mip_max + 1 : 1;
  const Key key = {base_page,
                   mip_page,
                   uint32_t(fetch.format),
                   width,
                   height,
                   uint32_t(fetch.pitch),
                   uint32_t(fetch.tiled) | (uint32_t(fetch.packed_mips) << 1) |
                       (uint32_t(fetch.endianness) << 2) | (levels << 4)};
  Texture* texture = GetTexture(fetch, host.format, key, levels);
  if (!texture) {
    return;
  }

  // The bytes it covers, hashed at most once a frame, and less often while they do not change (the
  // hash was a tenth of the ring thread on the Switch). Videos change every frame and stay checked.
  if (texture->checked_frame != frame_ && frame_ >= texture->next_check_frame) {
    texture->checked_frame = frame_;
    const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
        xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled, fetch.format,
        fetch.packed_mips, true, levels - 1);
    const uint32_t base_size = layout.base.level_data_extent_bytes;
    const uint32_t mip_size = layout.mips_total_extent_bytes;
    const uint8_t* physical = memory_->TranslatePhysical(0);
    uint64_t hash = XXH3_64bits(physical + (base_page << 12), base_size);
    if (mip_page && mip_size) {
      hash ^= XXH3_64bits(physical + (mip_page << 12), mip_size) * 31;
    }
    if (!texture->uploaded || hash != texture->content_hash) {
      before_commands();
      if (Upload(cb, fetch, *texture, width, height, base_page, mip_page)) {
        texture->content_hash = hash;
        texture->stable_checks = 0;
        texture->next_check_frame = frame_ + 1;
      } else {
        texture->checked_frame = UINT64_MAX;  // Upload memory full: try again in the next frame.
      }
    } else {
      texture->stable_checks = std::min(texture->stable_checks + 1, 4u);
      texture->next_check_frame = frame_ + (uint64_t(1) << texture->stable_checks);
    }
  }
  if (!texture->uploaded) {
    return;
  }
  out.view = GetView(texture->image, texture->format, texture->levels, fetch.swizzle, texture->views);
  out.sampler = GetSampler(fetch, texture->levels);
}

}  // namespace rex::graphics::native
