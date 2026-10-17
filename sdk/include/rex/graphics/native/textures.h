/**
 * @file        native/textures.h
 * @brief       COTT: textures of the native renderer (Phase 3, milestone M3c)
 *
 * A texture fetch constant names either the destination of an earlier resolve (render-to-texture: the
 * image NativeTargets keeps for that address is sampled directly) or texture data the game wrote to
 * memory. The latter is untiled and endian-swapped on the CPU into the frame's upload memory and copied
 * into a Vulkan image. Its bytes are hashed the first time it is used in a frame, and uploaded again when
 * they changed (videos, streaming); M4 can replace the hashing with write watches if it costs too much.
 *
 * Formats so far: k_8, k_8_8, k_8_8_8_8, DXT1/3/5 (BC1/2/3), k_16_16_16_16_FLOAT, k_32_FLOAT, and the
 * resolved k_8_8_8_8 and k_24_8(_FLOAT). Others are sampled as a black texture and logged once.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

class NativeTargets;
class UploadArena;

struct TextureBinding {
  VkImageView view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  // Multiplies the sampled value in the shader (g_TextureSampleScale): 2 for resolved D24FS8 depth, kept
  // halved on the host.
  float sample_scale = 1.0f;
};

class NativeTextures {
 public:
  NativeTextures() = default;
  ~NativeTextures();

  bool Initialize(ui::vulkan::VulkanDevice* device, const memory::Memory* memory,
                  UploadArena* upload);
  void Shutdown();

  // Records the dummy images' clears; once, before the first draw.
  void InitializeDummies(VkCommandBuffer cb);
  void BeginFrame() { ++frame_; }

  // What a shader samples for the fetch constant. Uploads and layout changes are recorded into `cb`;
  // `before_commands` is called first (the caller ends its rendering pass).
  void Prepare(VkCommandBuffer cb, const xenos::xe_gpu_texture_fetch_t& fetch, NativeTargets& targets,
               const std::function<void()>& before_commands, TextureBinding& out);

  // Bound where a shader samples nothing: 2D, 3D and cube.
  VkImageView dummy_view(uint32_t dimension) const { return dummies_[dimension].view; }
  VkSampler default_sampler() const { return default_sampler_; }

  uint64_t uploads() const { return uploads_; }
  uint64_t upload_bytes() const { return upload_bytes_; }

 private:
  struct Key {
    uint32_t base_page, mip_page, format, width, height, pitch, flags;
    bool operator==(const Key& other) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = k.base_page;
      for (uint32_t v : {k.mip_page, k.format, k.width, k.height, k.pitch, k.flags}) {
        h = (h ^ v) * 0x100000001B3ull;
      }
      return size_t(h);
    }
  };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t content_hash = 0;
    uint64_t checked_frame = UINT64_MAX;
    // Content that stayed the same is checked less often: every 2, 4, 8, then 16 frames.
    uint64_t next_check_frame = 0;
    uint32_t stable_checks = 0;
    bool uploaded = false;
    std::unordered_map<uint32_t, VkImageView> views;  // by swizzle
  };
  struct Dummy {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  Texture* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch, VkFormat format, const Key& key,
                      uint32_t levels);
  bool Upload(VkCommandBuffer cb, const xenos::xe_gpu_texture_fetch_t& fetch, Texture& texture,
              uint32_t width, uint32_t height, uint32_t base_page, uint32_t mip_page);
  VkImageView GetView(VkImage image, VkFormat format, uint32_t levels, uint32_t swizzle,
                      std::unordered_map<uint32_t, VkImageView>& views);
  VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t levels);
  void Unsupported(uint32_t format, const char* what);

  ui::vulkan::VulkanDevice* device_ = nullptr;
  const memory::Memory* memory_ = nullptr;
  UploadArena* upload_ = nullptr;
  uint64_t frame_ = 0;

  std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures_;
  // Swizzled views of resolve destinations, by image.
  std::unordered_map<VkImage, std::unordered_map<uint32_t, VkImageView>> resolved_views_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  VkSampler default_sampler_ = VK_NULL_HANDLE;
  Dummy dummies_[3];
  bool dummies_initialized_ = false;

  std::unordered_set<uint32_t> unsupported_logged_;
  uint64_t uploads_ = 0, upload_bytes_ = 0;
};

}  // namespace rex::graphics::native
