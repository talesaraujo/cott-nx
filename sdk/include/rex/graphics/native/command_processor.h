/**
 * @file        native/command_processor.h
 * @brief       COTT: the native renderer's command processor (see native/graphics_system.h)
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
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
    bool submitted = false;
  };
  static constexpr size_t kFrameSlots = 3;
  std::array<FrameSlot, kFrameSlots> slots_{};
  size_t slot_index_ = 0;

  // The command buffer of the frame being recorded: opened by the first resolve or draw after a swap,
  // submitted by the swap.
  VkCommandBuffer frame_command_buffer_ = VK_NULL_HANDLE;
  VkCommandBuffer EnsureFrame();
  void SubmitFrame();

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

  VkDescriptorSetLayout set_layouts_[4] = {};
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

  bool rendering_ = false;
  NativeImage* rendering_targets_[3] = {};  // color 0, color 1, depth
  VkExtent2D rendering_extent_ = {};

  // Scratch index lists, kept to avoid reallocating them every draw.
  std::vector<uint32_t> draw_indices_, draw_host_indices_;

  std::unordered_set<uint32_t> draw_skips_logged_;
  // Pixel shader texture slots already described in the log (hash ^ slot).
  std::unordered_set<uint64_t> traced_shaders_;
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
  std::chrono::steady_clock::time_point report_start_;
};

}  // namespace rex::graphics::native
