/**
 * @file        native/targets.h
 * @brief       COTT: render targets and resolves of the native renderer (Phase 3, milestone M3b)
 *
 * The Xenos GPU draws into 10 MB of on-chip memory (EDRAM) and "resolves" (copies) regions of it into
 * textures in main memory. The native renderer never imitates the EDRAM:
 *
 *  - Each render target the game binds becomes a Vulkan image, found by what the game binds it with:
 *    EDRAM base, pitch, format and MSAA mode. The image covers the EDRAM area from that base, in pixels.
 *    MSAA is drawn with one sample (on the Switch, NVK hangs with MSAA; nfsmw-nx made the same choice).
 *  - Each resolve destination becomes a Vulkan image too, found by its address in guest memory.
 *
 * Predicated tiling. Crash of the Titans draws its 1280x720 2xMSAA scene in three vertical strips of up to
 * 448 pixels, because color and depth do not fit in the EDRAM together. The game's Direct3D replays the
 * frame once per strip with a window offset, so each strip lands at the start of the same EDRAM surface,
 * and resolves it with RB_COPY_DEST_BASE moved to where that strip starts inside the destination texture.
 * The tiled layout makes that move non-linear, so the strip's x position is recovered by searching which
 * offset of an existing destination gives that address (see FindDestination).
 *
 * What is covered: color render targets k_8_8_8_8(_GAMMA) and depth k_24_8 / k_24_8_FLOAT; copies to
 * k_8_8_8_8 (color) and k_24_8(_FLOAT) (depth, as R32_SFLOAT); clears of the whole image; and the Swap
 * showing the destination texture at the front buffer address (M3b); draws into them (M3c).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
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

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::native {

struct NativeImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Destinations only: where the image lives in guest memory, and its tiled pitch and format.
  uint32_t guest_base = 0;
  uint32_t guest_pitch = 0;
  uint32_t guest_height = 0;
  uint32_t guest_format = 0;
  // Multiplier for sampling it as a texture: 2 for depth resolved from D24FS8 (host depth is halved).
  float sample_scale = 1.0f;
  // Color destinations resolved with RB_COPY_DEST_INFO::copy_dest_swap: in guest memory red and blue
  // trade places. The image keeps the render target's order; sampling swaps them (the game's fetch
  // swizzle swaps them back).
  bool swap_red_blue = false;
};

class NativeTargets {
 public:
  NativeTargets() = default;
  ~NativeTargets();

  bool Initialize(ui::vulkan::VulkanDevice* device);
  void Shutdown();

  // Performs the resolve described by the current registers: copy, then clears, as Xenos does.
  void Resolve(VkCommandBuffer cb, const RegisterFile& regs, const memory::Memory& memory);

  // The render targets a draw writes, as bound by RB_SURFACE_INFO, RB_COLOR_INFO / RB_COLOR1_INFO and
  // RB_DEPTH_INFO (same images the resolves read). Null when the format is not supported yet.
  NativeImage* GetColorTarget(const RegisterFile& regs, uint32_t index);
  NativeImage* GetDepthTarget(const RegisterFile& regs);

  // The destination texture whose guest base is exactly `base`, or null.
  NativeImage* FindTexture(uint32_t base);
  // The color destination of the most recent resolve (debug presentation).
  NativeImage* last_color_destination() const { return last_color_destination_; }

  // Moves the image to `layout` with a conservative full barrier (M3b keeps synchronization simple).
  void Transition(VkCommandBuffer cb, NativeImage& image, VkImageLayout layout);

  // Debugging: writes every render target and resolve destination to `dir` as PPM images. `begin`
  // opens a command buffer, `submit_and_wait` runs it to completion (the GPU must be idle before).
  void DumpImages(const std::string& dir, const std::function<VkCommandBuffer()>& begin,
                  const std::function<void()>& submit_and_wait);

  uint64_t resolves() const { return resolves_; }
  uint64_t skipped() const { return skipped_; }

 private:
  struct RenderTargetKey {
    uint32_t base_tiles;
    uint32_t pitch_tiles;
    uint32_t format;
    uint32_t msaa;
    uint32_t is_depth;
    bool operator==(const RenderTargetKey& other) const = default;
  };
  struct RenderTargetKeyHash {
    size_t operator()(const RenderTargetKey& key) const {
      return (size_t(key.base_tiles) << 20) ^ (size_t(key.pitch_tiles) << 8) ^
             (size_t(key.format) << 4) ^ (size_t(key.msaa) << 2) ^ key.is_depth;
    }
  };

  NativeImage* GetRenderTarget(const RenderTargetKey& key);
  // `image` and every other render target at the same EDRAM base (color or depth), in any MSAA mode.
  std::vector<NativeImage*> Aliases(NativeImage* image, uint32_t base_tiles, bool is_depth);
  // The destination texture a resolve writes to, and the x position (pixels) of the resolved strip
  // inside it.
  NativeImage* FindDestination(uint32_t base, uint32_t pitch, uint32_t height, uint32_t format,
                               uint32_t& strip_x_out);
  bool CreateImage(NativeImage& image, VkFormat format, VkImageAspectFlags aspect,
                   VkImageUsageFlags usage, uint32_t width, uint32_t height);
  void DestroyImage(NativeImage& image);
  bool EnsureStagingBuffer(VkDeviceSize size);
  // Logs a reason the first time it is seen.
  void Skip(uint32_t reason, const char* text);

  ui::vulkan::VulkanDevice* device_ = nullptr;
  std::unordered_map<RenderTargetKey, std::unique_ptr<NativeImage>, RenderTargetKeyHash> targets_;
  std::unordered_map<uint32_t, std::unique_ptr<NativeImage>> destinations_;
  NativeImage* last_color_destination_ = nullptr;

  // Depth goes to its R32_SFLOAT texture through this buffer (a depth aspect cannot be copied to a
  // color image directly).
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  VkDeviceSize staging_size_ = 0;

  std::unordered_set<uint32_t> skip_reasons_logged_;
  uint64_t resolves_ = 0;
  uint64_t skipped_ = 0;
};

}  // namespace rex::graphics::native
