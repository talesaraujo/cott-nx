/**
 * @file        native/command_processor.h
 * @brief       COTT: the native renderer's command processor (see native/graphics_system.h)
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <array>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/native/shader_library.h>
#include <rex/graphics/native/targets.h>
#include <rex/graphics/native/textures.h>
#include <rex/graphics/native/upload_arena.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  ui::vulkan::VulkanDevice* vulkan_device() const;

  // Frames in flight. Each slot has its own command pool, command buffer and fence, so the next frame is
  // recorded while the GPU works on the previous ones (the nfsmw-nx scheme, docs/native-renderer.md).
  struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // GPU timestamps: the first at the start of the frame's command buffer, the last at its end, and
    // one at each change of the kind of work in between (GpuMark).
    VkQueryPool timestamps = VK_NULL_HANDLE;
    std::vector<uint8_t> mark_kinds;  // kind of the work after each timestamp
    // Profile frames only: the draw after each timestamp (shaders and scissor size), for the GPU time
    // per shader pair in the report.
    struct DrawTag {
      uint64_t vs = 0, ps = 0;
      uint32_t width = 0, height = 0;
    };
    std::vector<DrawTag> mark_draws;
    bool profiled = false;
    bool submitted = false;
    std::chrono::steady_clock::time_point submit_time;
  };
  static constexpr size_t kFrameSlots = 3;
  std::array<FrameSlot, kFrameSlots> slots_{};
  size_t slot_index_ = 0;

  // The command buffer of the frame being recorded: opened by the first resolve or draw after a swap,
  // submitted by the swap.
  VkCommandBuffer frame_command_buffer_ = VK_NULL_HANDLE;
  VkCommandBuffer EnsureFrame();
  void SubmitFrame();

  // Predicated tiling drawn once (native_untile). The game draws its scene in vertical strips (the
  // window offset moves each to the start of the same EDRAM surface) and replays every draw that
  // touches a strip. The renderer has no EDRAM limit: the first strip is drawn as the whole screen
  // into render targets widened to it, its resolves copy the whole width, and the other strips' draws
  // and resolves are skipped. The strip layout is learned from the previous frame.
  // kFirstSurface: drawn while the first strip's bins are selected but without the strip's window: the
  // EDRAM clears D3D draws as rectangles in surface coordinates, which cover the whole strip.
  enum class Strip { kNone, kFirst, kOther, kFirstSurface };
  // By the window registers only (draws and resolves of the strips).
  Strip CurrentStrip();
  // Also by the selected bins (anything drawn while a strip is being replayed).
  Strip StripContext();
  bool ForcePredicatedPass() override;
  bool SkipIndirectBuffer(uint32_t ptr, uint32_t count) override;
  // From the previous frame: the later strips' widest window, where the second one starts, where the
  // first one ends, and the screen's width. Strips can differ: at 640x480 they are 0-224, 192-448 and
  // 416-640.
  uint32_t strip_width_ = 0, second_strip_x0_ = 0, first_strip_x1_ = 0, screen_width_ = 0;
  uint32_t strip_width_seen_ = 0, second_strip_x0_seen_ = 0, first_strip_x1_seen_ = 0,
           screen_width_seen_ = 0;
  uint64_t first_bins_ = 0, other_bins_ = 0;              // bin selections, previous frame
  uint64_t first_bins_seen_ = 0, other_bins_seen_ = 0;
  uint64_t strip_draws_skipped_ = 0, strip_resolves_skipped_ = 0;
  uint64_t strip_buffers_skipped_ = 0, strip_dwords_skipped_ = 0;
  static constexpr uint32_t kStripBufferSkipDwords = 256;
  // Draws per (window offset x, window scissor x0, x1, kind) in the report interval (diagnostics).
  std::map<std::tuple<int32_t, uint32_t, uint32_t, int>, uint32_t> strip_windows_;

  NativeTargets targets_;
  NativeShaderLibrary shader_library_;
  UploadArena upload_;
  NativeTextures textures_;

  // Draws (native/draw.cpp).
  bool CreateDrawObjects();
  void DestroyDrawObjects();
  void BeginSlotDrawState();
  bool Draw(xenos::PrimitiveType prim_type, uint32_t index_count,
            const IndexBufferInfo* index_buffer_info);
  void SkipDraw(uint32_t reason, const char* text);
  void CaptureMissingShader(const Shader& shader, bool is_pixel);
  // Ends the current rendering pass (before transfers, barriers and the end of the frame).
  void EndRendering();

  struct PipelineKey {
    uint64_t vs = 0, ps = 0;
    uint32_t topology = 0, color_mask = 0, blend0 = 0, blend1 = 0, depth_control = 0, raster = 0;
    uint32_t alpha_test = 0, attachments = 0, restart = 0, pad = 0;
    bool operator==(const PipelineKey& other) const = default;
  };
  struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const;
  };
  VkPipeline GetPipeline(const PipelineKey& key, const NativeShader& vs, const NativeShader* ps);
  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;

  // Sets 0-3: 2D, 3D and cube textures and samplers. Set 4: the constants as dynamic uniform buffers
  // (vertex, pixel, shared), one set per frame slot over its upload buffer.
  VkDescriptorSetLayout set_layouts_[5] = {};
  std::array<VkDescriptorSet, kFrameSlots> constant_sets_{};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  // Sets 1 and 2 (3D and cube textures: the game samples none) never change; sets 0 and 3 are written
  // per draw from the frame slot's pool.
  VkDescriptorPool static_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet static_sets_[2] = {};
  std::array<VkDescriptorPool, kFrameSlots> descriptor_pools_{};
  // The last texture bindings written, to reuse their sets when the next draw samples the same.
  std::array<VkImageView, 16> last_views_{};
  std::array<VkSampler, 16> last_samplers_{};
  VkDescriptorSet last_sets_[2] = {};

  // The last shared constants block uploaded in this frame, reused while a draw's is the same.
  std::array<uint8_t, 1024> shared_scratch_{}, last_shared_{};
  VkDeviceSize last_shared_offset_ = VK_WHOLE_SIZE;

  bool rendering_ = false;
  NativeImage* rendering_targets_[3] = {};  // color 0, color 1, depth
  VkExtent2D rendering_extent_ = {};

  // Scratch index lists, kept to avoid reallocating them every draw.
  std::vector<uint32_t> draw_indices_, draw_host_indices_;

  std::unordered_set<uint32_t> draw_skips_logged_;
  // Debugging: log every draw of the frame after a dump (native_dump_trigger).
  bool trace_frame_ = false, trace_next_frame_ = false;
  uint32_t trace_draw_index_ = 0;
  // Pixel shader texture slots already described in the log (hash ^ slot).
  std::unordered_set<uint64_t> traced_shaders_;
  // Missing shaders already saved (native_shader_capture_path).
  std::unordered_set<uint64_t> captured_shaders_;
  uint64_t draws_done_ = 0, draws_skipped_ = 0, report_draws_done_ = 0;

  bool CreateFrameSlots();
  void DestroyFrameSlots();
  // Waits until the slot is free and opens its command buffer.
  VkCommandBuffer BeginSlot();
  bool SubmitSlot();

  // Every distinct microcode the game loads, by the XXH3 of its guest bytes: the same key as the shader
  // storage (.xsh) and the SPIR-V library.
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;

  // Counters for the periodic report, written only by the command processor thread.
  uint64_t draws_ = 0, copies_ = 0, swaps_ = 0;
  uint64_t report_draws_ = 0, report_copies_ = 0, report_swaps_ = 0;
  uint64_t frame_first_draw_ = 0;  // draws_ at the frame's start (the copy trace)
  std::chrono::steady_clock::time_point report_start_;
  // Where a frame's time goes (the report): GPU time between the frame's two timestamps, the ring
  // thread's waits for a free slot, its time recording a frame (first command to submit), and the
  // time from a frame's submit to the end of its fence wait.
  PFN_vkCmdWriteTimestamp cmd_write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults get_query_pool_results_ = nullptr;
  double timestamp_period_ns_ = 0.0;
  uint64_t timestamp_mask_ = 0;
  // Kinds of GPU work: draws, resolve copies, resolve clears, texture uploads, the present blit.
  static constexpr uint32_t kGpuKinds = 5;
  static constexpr uint32_t kMaxGpuMarks = 4096;
  void GpuMark(uint32_t kind);
  // A timestamp before a draw of a profile frame (one frame after each report).
  void GpuMarkDraw(uint64_t vs, uint64_t ps, uint32_t width, uint32_t height);
  void GpuMarkResolve(uint32_t width, uint32_t height, uint32_t flags);
  bool profile_next_frame_ = false, profile_frame_ = false;
  std::vector<uint64_t> gpu_ticks_;
  uint32_t gpu_kind_ = 0;
  double gpu_kind_ns_[kGpuKinds] = {};
  double gpu_gap_ns_ = 0.0;
  uint64_t last_gpu_end_ = 0;
  double gpu_ns_ = 0.0, slot_wait_ns_ = 0.0, record_ns_ = 0.0;
  // Real nanoseconds per reported one. The Switch's GPU timestamps do not run at the rate the driver
  // reports (timestampPeriod): every measured span came out 1/1.627 of the real one. Each report
  // compares the GPU timeline (frames plus the gaps between them) with the CPU clock.
  double timestamp_scale_ = 1.0;
  uint64_t gpu_frames_ = 0;
  std::chrono::steady_clock::time_point record_start_;
};

}  // namespace rex::graphics::native
