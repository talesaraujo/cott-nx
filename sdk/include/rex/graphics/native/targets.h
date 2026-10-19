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
 * Resolution scale (native_resolution_scale): render targets and resolve destinations are Vulkan
 * images that many times the size the game asks for (0.8: 1280x720 drawn at 1024x576). Everything else
 * stays in guest pixels (NativeImage::width and height too) and is converted at the Vulkan calls, edge
 * by edge with ToHost, so rectangles that touch in guest pixels still touch. Pixel shaders get the
 * inverse scale (g_PixelPosScale) to turn their pixel position back into guest pixels.
 * The scale can change between frames (native_dynamic_resolution, NativeTargets::PrepareRescale):
 * every image is then made again at the new size, the color ones stretched into it.
 *
 * What is covered: color render targets k_8_8_8_8(_GAMMA) and depth k_24_8 / k_24_8_FLOAT; copies to
 * k_8_8_8_8 (color) and k_24_8(_FLOAT) (depth, as R32_SFLOAT); clears of the whole image; and the Swap
 * showing the destination texture at the front buffer address (M3b); draws into them (M3c).
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <thread>
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
  // In guest pixels; the Vulkan image is host_width x host_height (the resolution scale).
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageUsageFlags usage = 0;
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
  // Render targets only: the image's MSAA mode (1X or 2X; 4X surfaces draw into one of those) and when
  // a draw or resolve last used it with its own mode.
  uint32_t msaa = 0;
  uint64_t last_use = 0;
  // Depth render targets only: a depth-aspect view and the descriptor set the depth copy samples it
  // through (created on its first depth resolve).
  VkImageView depth_view = VK_NULL_HANDLE;
  VkDescriptorSet depth_copy_set = VK_NULL_HANDLE;
  // Destinations only: the rectangles copied in since a draw last sampled it (the copy statistics).
  std::vector<VkRect2D> unread;
};

// The current resolution scale, 0.5-1: native_resolution_scale at the start, then what
// NativeTargets::FinishRescale set.
float ResolutionScale();
// A guest pixel edge (x, or x + width) in images of that scale.
inline uint32_t ToHostAt(uint32_t guest, float scale) {
  return scale == 1.0f ? guest : uint32_t(float(guest) * scale + 0.5f);
}
// A guest pixel edge (x, or x + width) in the scaled images.
inline uint32_t ToHost(uint32_t guest) { return ToHostAt(guest, ResolutionScale()); }
inline int32_t ToHost(int32_t guest) {
  return guest < 0 ? -int32_t(ToHost(uint32_t(-guest))) : int32_t(ToHost(uint32_t(guest)));
}
// A guest rectangle in the scaled images.
inline VkRect2D ToHost(int32_t x, int32_t y, uint32_t width, uint32_t height) {
  const int32_t x0 = ToHost(x), y0 = ToHost(y);
  const int32_t x1 = ToHost(x + int32_t(width)), y1 = ToHost(y + int32_t(height));
  return {{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
}

class NativeTargets {
 public:
  NativeTargets() = default;
  ~NativeTargets();

  bool Initialize(ui::vulkan::VulkanDevice* device);
  void Shutdown();

  // Performs the resolve described by the current registers: copy, then clears, as Xenos does.
  // `full_width`: the first strip of a predicated tiling pass drawn as the whole screen (native_untile);
  // a rectangle from the strip's left edge is widened to that many pixels.
  void Resolve(VkCommandBuffer cb, const RegisterFile& regs, const memory::Memory& memory,
               uint32_t full_width = 0);
  // While not 0, the render targets looked up are at least this wide: an existing narrower image is
  // replaced (its contents are lost; the old one is destroyed once the GPU is past it).
  // Only images at least `widen_from` pixels wide are widened (the strip's; smaller targets drawn
  // during the strip are something else).
  uint32_t min_width = 0, widen_from = 0;
  // Called before an image is replaced, so the rendering pass that uses it ends first.
  std::function<void()> before_replace;
  // Once per frame: destroys the images replaced long enough ago.
  void RetireTick();
  // A new resolution scale, in two steps, because the Switch's driver clears every new allocation on
  // the CPU (making all the images again took 27-100 ms there, several frames):
  //  - PrepareRescale makes every render target and resolve destination again at `scale` (0.5-1) on a
  //    worker thread, while the frames keep using the old ones. False if one is under way already.
  //  - Between frames (one frame uses one scale), once rescale_ready, FinishRescale switches to them:
  //    the color images are stretched into the new ones, recorded into `cb`; depth is lost (the game
  //    clears it every frame). Images the game made meanwhile are made there. Returns how many images
  //    changed.
  bool PrepareRescale(float scale);
  bool rescale_under_way() const { return rescale_thread_.joinable(); }
  bool rescale_ready() const {
    return rescale_thread_.joinable() && rescale_done_.load(std::memory_order_acquire);
  }
  uint32_t FinishRescale(VkCommandBuffer cb);
  // Called with each image about to be destroyed (the texture cache drops its views of it).
  std::function<void(VkImage)> on_destroy_image;
  // Repeated color copies (native_coalesce_copies). The level's distortion particles each draw and then
  // copy the whole screen again, so the next one refracts the previous: a copy identical to the last one
  // (same registers, no clears) at most kCoalesceDraws draws later is deferred, and only the last of a
  // run is made, before anything else that could depend on it: another resolve, a later draw, the end of
  // the frame. The particles of a run see the screen as it was before the run.
  void FlushPendingCopy(VkCommandBuffer cb);
  bool has_pending_copy() const { return pending_copy_; }
  uint32_t last_copy_draw() const { return last_copy_draw_; }
  static constexpr uint32_t kCoalesceDraws = 2;
  uint64_t coalesced_copies() const { return coalesced_copies_; }
  // GPU time accounting (the command processor's report): called with 1 before a resolve's copy and
  // with 2 before its clears.
  std::function<void(uint32_t)> gpu_mark;
  // Profiled frames: one timestamp per resolve, tagged with its rectangle (width, height) and flags
  // (bit 0 depth copy, bit 1 has a copy, bit 2 clears color, bit 3 clears depth, bits 8+ MSAA samples).
  std::function<void(uint32_t width, uint32_t height, uint32_t flags)> gpu_mark_resolve;

  // The render targets a draw writes, as bound by RB_SURFACE_INFO, RB_COLOR_INFO / RB_COLOR1_INFO and
  // RB_DEPTH_INFO (same images the resolves read). Null when the format is not supported yet.
  NativeImage* GetColorTarget(const RegisterFile& regs, uint32_t index);
  NativeImage* GetDepthTarget(const RegisterFile& regs);
  // The RGBA8 color render targets that start at an EDRAM base with a given image width (the depth
  // surface's, so the tile layout matches).
  std::vector<NativeImage*> ColorImagesAt(uint32_t base_tiles, uint32_t width);

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

  // Copy statistics for the command processor's report: copies per frame and those overwritten by a
  // later copy before any draw read them. Logs and resets.
  void ReportCopies(uint64_t frames);
  // Profiled frames: one log line per copy, with the draw count before it.
  bool trace_copies = false;
  uint32_t trace_draw = 0;

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

  NativeImage* GetRenderTarget(const RenderTargetKey& requested);
  // `image` and every other render target at the same EDRAM base (color or depth), in any MSAA mode.
  std::vector<NativeImage*> Aliases(NativeImage* image, uint32_t base_tiles, bool is_depth);
  // The destination texture a resolve writes to, and the x position (pixels) of the resolved strip
  // inside it.
  NativeImage* FindDestination(uint32_t base, uint32_t pitch, uint32_t height, uint32_t format,
                               uint32_t& strip_x_out);
  bool CreateImage(NativeImage& image, VkFormat format, VkImageAspectFlags aspect,
                   VkImageUsageFlags usage, uint32_t width, uint32_t height);
  // The same at a given scale; touches nothing else (the rescale's worker thread calls it).
  bool CreateImageAt(NativeImage& image, VkFormat format, VkImageAspectFlags aspect,
                     VkImageUsageFlags usage, uint32_t width, uint32_t height, float scale) const;
  // The images PrepareRescale makes, for the ones in targets_ and destinations_.
  struct Prepared {
    NativeImage* owner;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    uint32_t width, height;
    NativeImage image;
  };
  std::vector<Prepared> prepared_;
  std::thread rescale_thread_;
  std::atomic<bool> rescale_done_{false};
  float rescale_scale_ = 1.0f;
  void DestroyImage(NativeImage& image);
  void Widen(NativeImage& image, uint32_t width);
  struct Retired {
    NativeImage image;
    uint32_t age = 0;
  };
  std::vector<Retired> retired_;
  bool EnsureStagingBuffer(VkDeviceSize size);
  // Depth resolves through a draw: a fragment shader reads the depth (texelFetch) and writes it to the
  // R32_SFLOAT destination. Through the staging buffer (image to buffer to image) each 448x720 depth
  // resolve cost ~1.4 ms of GPU time on the Switch, ~16 ms per frame in the level. False when the
  // pipeline could not be made (the buffer path is used then).
  bool EnsureDepthCopy();
  bool DepthCopy(VkCommandBuffer cb, NativeImage& src, NativeImage& dest, uint32_t src_x,
                 uint32_t src_y, uint32_t dest_x, uint32_t dest_y, uint32_t width, uint32_t height);
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

  VkDescriptorSetLayout depth_copy_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_copy_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_copy_pipeline_ = VK_NULL_HANDLE;
  VkSampler depth_copy_sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_copy_pool_ = VK_NULL_HANDLE;
  // Sets of destroyed images (the GPU is past them), for the next depth render targets.
  std::vector<VkDescriptorSet> free_depth_copy_sets_;
  bool depth_copy_failed_ = false;

  // The last color copy made (for native_coalesce_copies) and whether a copy like it is pending.
  std::array<uint32_t, 12> last_copy_signature_{};
  bool last_copy_valid_ = false, pending_copy_ = false;
  NativeImage* last_copy_src_ = nullptr;
  NativeImage* last_copy_dest_ = nullptr;
  VkImageCopy last_copy_region_{};
  uint32_t last_copy_draw_ = 0;
  uint64_t coalesced_copies_ = 0;

  std::unordered_set<uint32_t> skip_reasons_logged_;
  uint64_t resolves_ = 0;
  uint64_t color_copies_ = 0, depth_copies_ = 0, unread_overwritten_ = 0, copied_pixels_ = 0;
  uint64_t unread_pixels_ = 0;
  uint64_t skipped_ = 0;
  uint64_t use_counter_ = 0;
};

}  // namespace rex::graphics::native
