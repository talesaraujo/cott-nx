/**
 * @file        native/command_processor.cpp
 * @brief       COTT: the native renderer's command processor, milestone M2 (see native/graphics_system.h)
 */
#include <rex/graphics/native/command_processor.h>

#include <xxhash.h>

#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(native_dump_trigger, "", "GPU/Native",
                      "Debugging: when this file appears, the images of the next frame are written "
                      "next to it (as PPM) and the file is deleted");
REXCVAR_DEFINE_BOOL(native_present_last_resolve, false, "GPU/Native",
                    "Show the destination of the latest color resolve instead of the front buffer "
                    "(debugging the render targets before draws work)");

#if REX_PLATFORM_SWITCH
// Game FPS for the console profiler (switch_perf.cpp): one count per guest present, as in the emulation.
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace rex::graphics::native {

namespace {
constexpr auto kReportInterval = std::chrono::seconds(10);
}  // namespace

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("[native] base command processor context failed");
    return false;
  }
  if (!CreateFrameSlots() || !targets_.Initialize(vulkan_device()) || !CreateDrawObjects()) {
    return false;
  }
  report_start_ = std::chrono::steady_clock::now();
  REXGPU_INFO("[native] native renderer, milestone M3c: draws with the translated shaders");
  return true;
}

ui::vulkan::VulkanDevice* NativeCommandProcessor::vulkan_device() const {
  return static_cast<const ui::vulkan::VulkanProvider*>(graphics_system_->provider())
      ->vulkan_device();
}

bool NativeCommandProcessor::CreateFrameSlots() {
  const ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  for (FrameSlot& slot : slots_) {
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device->device(), &pool_info, nullptr, &slot.pool) != VK_SUCCESS) {
      REXGPU_ERROR("[native] vkCreateCommandPool failed");
      return false;
    }
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = slot.pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(device->device(), &alloc_info, &slot.command_buffer) !=
        VK_SUCCESS) {
      REXGPU_ERROR("[native] vkAllocateCommandBuffers failed");
      return false;
    }
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (dfn.vkCreateFence(device->device(), &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
      REXGPU_ERROR("[native] vkCreateFence failed");
      return false;
    }
  }
  return true;
}

void NativeCommandProcessor::DestroyFrameSlots() {
  const ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  for (FrameSlot& slot : slots_) {
    if (slot.submitted) {
      dfn.vkWaitForFences(device->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    }
    if (slot.fence) dfn.vkDestroyFence(device->device(), slot.fence, nullptr);
    if (slot.pool) dfn.vkDestroyCommandPool(device->device(), slot.pool, nullptr);
    slot = FrameSlot{};
  }
}

VkCommandBuffer NativeCommandProcessor::BeginSlot() {
  const ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  FrameSlot& slot = slots_[slot_index_];
  if (slot.submitted) {
    dfn.vkWaitForFences(device->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(device->device(), 1, &slot.fence);
    slot.submitted = false;
  }
  dfn.vkResetCommandPool(device->device(), slot.pool, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info);
  return slot.command_buffer;
}

bool NativeCommandProcessor::SubmitSlot() {
  ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  FrameSlot& slot = slots_[slot_index_];
  dfn.vkEndCommandBuffer(slot.command_buffer);
  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot.command_buffer;
  VkResult result;
  {
    ui::vulkan::VulkanDevice::Queue::Acquisition queue =
        device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    result = dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, slot.fence);
  }
  slot_index_ = (slot_index_ + 1) % kFrameSlots;
  if (result != VK_SUCCESS) {
    REXGPU_ERROR("[native] vkQueueSubmit failed ({})", int(result));
    if (result == VK_ERROR_DEVICE_LOST && graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(true);
    }
    return false;
  }
  slot.submitted = true;
  return true;
}

VkCommandBuffer NativeCommandProcessor::EnsureFrame() {
  if (!frame_command_buffer_) {
    frame_command_buffer_ = BeginSlot();
    BeginSlotDrawState();
  }
  return frame_command_buffer_;
}

void NativeCommandProcessor::SubmitFrame() {
  if (frame_command_buffer_) {
    EndRendering();
    SubmitSlot();
    frame_command_buffer_ = VK_NULL_HANDLE;
  }
}

void NativeCommandProcessor::ShutdownContext() {
  SubmitFrame();
  DestroyFrameSlots();
  DestroyDrawObjects();
  targets_.Shutdown();
  REXGPU_INFO("[native] {} draws, {} copies and {} swaps in total; {} distinct shaders", draws_,
              copies_, swaps_, shaders_.size());
  shaders_.clear();
  CommandProcessor::ShutdownContext();
}

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t /*guest_address*/,
                                           const uint32_t* host_address, uint32_t dword_count) {
  // Same hash as the emulation's pipeline cache (XXH3 of the guest bytes, big-endian as in memory).
  const uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  REXGPU_DEBUG("[native] new {} shader {:016X} ({} dwords)",
               shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool /*major_mode_explicit*/) {
  // A resolve (EDRAM to memory copy) arrives as a draw with RB_MODECONTROL in copy mode, as in the
  // emulation (VulkanCommandProcessor::IssueDraw).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++draws_;
  return Draw(prim_type, index_count, index_buffer_info);
}

bool NativeCommandProcessor::IssueCopy() {
  ++copies_;
  VkCommandBuffer cb = EnsureFrame();
  EndRendering();
  targets_.Resolve(cb, *register_file_, *memory_);
  return true;
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  ++swaps_;
  trace_frame_ = trace_next_frame_;
  trace_next_frame_ = false;
  trace_draw_index_ = 0;

  // The image the game shows is the resolve destination at the front buffer address. The presenter
  // hands over its guest output image; the frame's commands and the blit into it are submitted before
  // the refresher returns, because the presenter paints right after it, on this thread ("presentando en
  // el acto"), on the same queue. Without a known front buffer (the very first frames), a test color.
  NativeImage* source = REXCVAR_GET(native_present_last_resolve)
                            ? targets_.last_color_destination()
                            : targets_.FindTexture(frontbuffer_ptr);
  if (ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr) {
    const uint32_t width = frontbuffer_width ? frontbuffer_width : 1280;
    const uint32_t height = frontbuffer_height ? frontbuffer_height : 720;
    presenter->RefreshGuestOutput(
        width, height, 1280, 720,
        [this, source, width, height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vk_context =
              static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device()->functions();
          VkCommandBuffer cb = EnsureFrame();
          EndRendering();
          const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();

          VkImageMemoryBarrier barrier = {};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask = vk_context.image_ever_written_previously()
                                      ? ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                      : 0;
          barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vk_context.image();
          barrier.subresourceRange = range;
          dfn.vkCmdPipelineBarrier(cb,
                                   vk_context.image_ever_written_previously()
                                       ? VkPipelineStageFlags(ui::vulkan::VulkanPresenter::
                                                                  kGuestOutputInternalStageMask)
                                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                   &barrier);

          if (source) {
            targets_.Transition(cb, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit blit = {};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(std::min(width, source->width)),
                                  int32_t(std::min(height, source->height)), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
            dfn.vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               vk_context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_NEAREST);
          } else {
            const float t = float(swaps_ % 360) * (6.2831853f / 360.0f);
            VkClearColorValue color = {};
            color.float32[0] = 0.5f + 0.5f * std::sin(t);
            color.float32[1] = 0.5f + 0.5f * std::sin(t + 2.094f);
            color.float32[2] = 0.5f + 0.5f * std::sin(t + 4.189f);
            color.float32[3] = 1.0f;
            dfn.vkCmdClearColorImage(cb, vk_context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &color, 1, &range);
          }

          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.newLayout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
          dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0,
                                   nullptr, 0, nullptr, 1, &barrier);
          context.SetIs8bpc(true);
          SubmitFrame();
          return true;
        });
  }
  // Not painted (no presenter, minimized window): the frame's work still has to go.
  SubmitFrame();

  // Debugging: dump this frame's images once the trigger file appears.
  const std::string& trigger = REXCVAR_GET(native_dump_trigger);
  std::error_code error;
  if (!trigger.empty() && std::filesystem::exists(trigger, error)) {
    std::filesystem::remove(trigger, error);
    trace_next_frame_ = true;
    const ui::vulkan::VulkanDevice* const device = vulkan_device();
    for (FrameSlot& slot : slots_) {
      if (slot.submitted) {
        device->functions().vkWaitForFences(device->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
      }
    }
    const std::string dir = std::filesystem::path(trigger).parent_path().string();
    targets_.DumpImages(
        dir.empty() ? "." : dir,
        [this]() { return EnsureFrame(); },
        [this, device]() {
          const size_t slot = slot_index_;
          SubmitFrame();
          device->functions().vkWaitForFences(device->device(), 1, &slots_[slot].fence, VK_TRUE,
                                              UINT64_MAX);
        });
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - report_start_ >= kReportInterval) {
    const double seconds = std::chrono::duration<double>(now - report_start_).count();
    const uint64_t swaps = swaps_ - report_swaps_;
    REXGPU_INFO("[native] {:.1f} swaps/s, {:.0f} draws ({:.0f} drawn) and {:.1f} copies per frame; "
                "{} guest shaders, {} translated, {} pipelines; {} texture uploads ({} MB); "
                "{} draws and {} resolves skipped in total; upload {} of {} MB (front buffer "
                "{:08X})",
                swaps / seconds, swaps ? double(draws_ - report_draws_) / swaps : 0.0,
                swaps ? double(draws_done_ - report_draws_done_) / swaps : 0.0,
                swaps ? double(copies_ - report_copies_) / swaps : 0.0, shaders_.size(),
                shader_library_.loaded(), pipelines_.size(), textures_.uploads(),
                textures_.upload_bytes() >> 20, draws_skipped_, targets_.skipped(),
                upload_.used() >> 20, upload_.capacity() >> 20, frontbuffer_ptr);
    report_draws_done_ = draws_done_;
    report_start_ = now;
    report_draws_ = draws_;
    report_copies_ = copies_;
    report_swaps_ = swaps_;
  }
}

}  // namespace rex::graphics::native
