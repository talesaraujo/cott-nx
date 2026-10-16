/**
 * @file        native/upload_arena.cpp
 * @brief       COTT: per-frame upload memory of the native renderer (see the header)
 */
#include <rex/graphics/native/upload_arena.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

UploadArena::~UploadArena() {
  Shutdown();
}

bool UploadArena::Initialize(ui::vulkan::VulkanDevice* device, VkDeviceSize bytes_per_slot) {
  device_ = device;
  bytes_per_slot_ = bytes_per_slot;
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  // vkGetBufferDeviceAddress is Vulkan 1.2; the device function table only lists what the emulation
  // uses, so it is fetched here.
  auto get_address = PFN_vkGetBufferDeviceAddress(
      device->vulkan_instance()->functions().vkGetDeviceProcAddr(device->device(),
                                                                 "vkGetBufferDeviceAddress"));
  if (!get_address) {
    REXGPU_ERROR("[native] vkGetBufferDeviceAddress is not available (Vulkan 1.2 is required)");
    return false;
  }
  for (Slot& slot : slots_) {
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes_per_slot;
    buffer_info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn.vkCreateBuffer(device->device(), &buffer_info, nullptr, &slot.buffer) != VK_SUCCESS) {
      REXGPU_ERROR("[native] could not create the upload buffer");
      return false;
    }
    VkMemoryRequirements requirements;
    dfn.vkGetBufferMemoryRequirements(device->device(), slot.buffer, &requirements);
    // Host visible and coherent, so writes need no flush.
    const ui::vulkan::VulkanDevice::MemoryTypes& types = device->memory_types();
    uint32_t type_index = UINT32_MAX;
    const uint32_t candidates = requirements.memoryTypeBits & types.host_visible & types.host_coherent;
    for (uint32_t pass = 0; pass < 2 && type_index == UINT32_MAX; ++pass) {
      // Prefer memory the GPU reads fast (device local and host visible: ReBAR on the PC, everything on
      // the Switch), then any coherent memory.
      const uint32_t mask = pass == 0 ? (candidates & types.device_local) : candidates;
      for (uint32_t i = 0; i < 32; ++i) {
        if (mask & (1u << i)) {
          type_index = i;
          break;
        }
      }
    }
    if (type_index == UINT32_MAX) {
      REXGPU_ERROR("[native] no host-visible coherent memory for the upload buffer");
      return false;
    }
    VkMemoryAllocateFlagsInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &flags_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type_index;
    if (dfn.vkAllocateMemory(device->device(), &allocate_info, nullptr, &slot.memory) != VK_SUCCESS ||
        dfn.vkBindBufferMemory(device->device(), slot.buffer, slot.memory, 0) != VK_SUCCESS ||
        dfn.vkMapMemory(device->device(), slot.memory, 0, VK_WHOLE_SIZE, 0,
                        reinterpret_cast<void**>(&slot.mapping)) != VK_SUCCESS) {
      REXGPU_ERROR("[native] could not allocate {} MB of upload memory", bytes_per_slot >> 20);
      return false;
    }
    VkBufferDeviceAddressInfo address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = slot.buffer;
    slot.address = get_address(device->device(), &address_info);
  }
  REXGPU_INFO("[native] upload memory: {} x {} MB", kSlots, bytes_per_slot >> 20);
  return true;
}

void UploadArena::Shutdown() {
  if (!device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  for (Slot& slot : slots_) {
    if (slot.buffer) dfn.vkDestroyBuffer(device_->device(), slot.buffer, nullptr);
    if (slot.memory) dfn.vkFreeMemory(device_->device(), slot.memory, nullptr);
    slot = Slot{};
  }
  device_ = nullptr;
}

void UploadArena::BeginSlot(size_t slot) {
  slot_ = slot % kSlots;
  used_ = 0;
}

uint8_t* UploadArena::Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out) {
  const VkDeviceSize offset = (used_ + alignment - 1) / alignment * alignment;
  if (offset + size > bytes_per_slot_) {
    ++exhausted_;
    return nullptr;
  }
  used_ = offset + size;
  offset_out = offset;
  return slots_[slot_].mapping + offset;
}

}  // namespace rex::graphics::native
