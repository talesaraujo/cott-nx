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

#include <array>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/native/targets.h>
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
