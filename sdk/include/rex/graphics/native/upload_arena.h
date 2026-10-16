/**
 * @file        native/upload_arena.h
 * @brief       COTT: per-frame upload memory of the native renderer (Phase 3, milestone M3c)
 *
 * One host-visible buffer per frame slot, filled linearly while the frame is recorded and reused when
 * the slot's fence says the GPU is done with it. It holds everything a draw reads from the CPU side:
 * the shader constants (read through buffer device addresses), vertex and index data converted from
 * big endian, and the staging copies of guest textures.
 */
#pragma once

#include <array>
#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

class UploadArena {
 public:
  static constexpr size_t kSlots = 3;

  UploadArena() = default;
  ~UploadArena();

  bool Initialize(ui::vulkan::VulkanDevice* device, VkDeviceSize bytes_per_slot);
  void Shutdown();

  // Starts filling the slot's buffer from the beginning (its previous frame has completed).
  void BeginSlot(size_t slot);

  // Space for `size` bytes, or null when the slot is full (the caller skips that work this frame).
  uint8_t* Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out);

  VkBuffer buffer() const { return slots_[slot_].buffer; }
  VkDeviceAddress address() const { return slots_[slot_].address; }
  VkDeviceSize used() const { return used_; }
  VkDeviceSize capacity() const { return bytes_per_slot_; }
  uint64_t exhausted() const { return exhausted_; }

 private:
  struct Slot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceAddress address = 0;
  };

  ui::vulkan::VulkanDevice* device_ = nullptr;
  std::array<Slot, kSlots> slots_{};
  size_t slot_ = 0;
  VkDeviceSize bytes_per_slot_ = 0;
  VkDeviceSize used_ = 0;
  uint64_t exhausted_ = 0;
};

}  // namespace rex::graphics::native
