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
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(native_dump_trigger, "", "GPU/Native",
                      "Debugging: when this file appears, the images of the next frame are written "
                      "next to it (as PPM) and the file is deleted");
REXCVAR_DEFINE_BOOL(native_gpu_marks, true, "GPU/Native",
                    "GPU timestamps at every change of work kind (draws, resolves, clears, uploads) in every "
                    "frame. Each one makes the GPU wait for the work before it. Off: only the frame's start "
                    "and end, and the profiled frame every report.");
REXCVAR_DEFINE_BOOL(native_present_last_resolve, false, "GPU/Native",
                    "Show the destination of the latest color resolve instead of the front buffer "
                    "(debugging the render targets before draws work)");

#if REX_PLATFORM_SWITCH
// Game FPS for the console profiler (switch_perf.cpp): one count per guest present, as in the emulation.
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace rex::graphics::native {

// Resolve tags have vs 0 (the per-shader sums skip them) and the flags in ps.
constexpr uint64_t kResolveTag = 1ull << 63;

namespace {
constexpr auto kReportInterval = std::chrono::seconds(10);
}  // namespace

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {
  // No WriteRegister override: the registers from kFirstPlainRegister up are read at draw time.
  plain_register_writes_ = true;
}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("[native] base command processor context failed");
    return false;
  }
  targets_.gpu_mark = [this](uint32_t kind) { GpuMark(kind); };
  targets_.gpu_mark_resolve = [this](uint32_t width, uint32_t height, uint32_t flags) {
    GpuMarkResolve(width, height, flags);
  };
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
  // GPU timestamps for the report. The device function table leaves these entry points out.
  {
    const auto& ifn = device->vulkan_instance()->functions();
    cmd_write_timestamp_ =
        PFN_vkCmdWriteTimestamp(ifn.vkGetDeviceProcAddr(device->device(), "vkCmdWriteTimestamp"));
    get_query_pool_results_ = PFN_vkGetQueryPoolResults(
        ifn.vkGetDeviceProcAddr(device->device(), "vkGetQueryPoolResults"));
    VkPhysicalDeviceProperties properties;
    ifn.vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
    timestamp_period_ns_ = properties.limits.timestampPeriod;
    uint32_t family_count = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(device->physical_device(), &family_count,
                                                 families.data());
    const uint32_t family = device->queue_family_graphics_compute();
    const uint32_t bits = family < family_count ? families[family].timestampValidBits : 0;
    timestamp_mask_ = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
    if (!bits || !cmd_write_timestamp_ || !get_query_pool_results_) {
      cmd_write_timestamp_ = nullptr;
      REXGPU_WARN("[native] no GPU timestamps: the report leaves GPU time out");
    }
  }
  for (FrameSlot& slot : slots_) {
    if (cmd_write_timestamp_) {
      VkQueryPoolCreateInfo query_info = {};
      query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query_info.queryCount = kMaxGpuMarks;
      if (dfn.vkCreateQueryPool(device->device(), &query_info, nullptr, &slot.timestamps) !=
          VK_SUCCESS) {
        slot.timestamps = VK_NULL_HANDLE;
      }
    }
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
    if (slot.timestamps) dfn.vkDestroyQueryPool(device->device(), slot.timestamps, nullptr);
    if (slot.pool) dfn.vkDestroyCommandPool(device->device(), slot.pool, nullptr);
    slot = FrameSlot{};
  }
}

VkCommandBuffer NativeCommandProcessor::BeginSlot() {
  const ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  FrameSlot& slot = slots_[slot_index_];
  if (slot.submitted) {
    const auto wait_start = std::chrono::steady_clock::now();
    dfn.vkWaitForFences(device->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    slot_wait_ns_ += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() -
                                                               wait_start)
                         .count();
    dfn.vkResetFences(device->device(), 1, &slot.fence);
    slot.submitted = false;
    const uint32_t marks = uint32_t(slot.mark_kinds.size());
    gpu_ticks_.resize(kMaxGpuMarks);
    uint64_t* ticks = gpu_ticks_.data();
    if (slot.timestamps && marks >= 2) {
      if (get_query_pool_results_(device->device(), slot.timestamps, 0, marks,
                                  sizeof(uint64_t) * marks, ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        auto ns_between = [this](uint64_t from, uint64_t to) {
          return double((to - from) & timestamp_mask_) * timestamp_period_ns_ * timestamp_scale_;
        };
        gpu_ns_ += ns_between(ticks[0], ticks[marks - 1]);
        for (uint32_t i = 0; i + 1 < marks; ++i) {
          gpu_kind_ns_[slot.mark_kinds[i]] += ns_between(ticks[i], ticks[i + 1]);
        }
        // GPU idle (or busy with other submissions, like the presenter's) between frames.
        if (last_gpu_end_) gpu_gap_ns_ += ns_between(last_gpu_end_, ticks[0]);
        last_gpu_end_ = ticks[marks - 1];
        ++gpu_frames_;
        if (slot.profiled) {
          // GPU time per shader pair in this frame, the heaviest first.
          struct Sum {
            double ns = 0.0;
            uint32_t draws = 0, width = 0, height = 0;
          };
          std::unordered_map<uint64_t, Sum> sums;
          std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> pairs;
          double total = 0.0;
          for (uint32_t i = 0; i + 1 < marks && i < slot.mark_draws.size(); ++i) {
            const FrameSlot::DrawTag& tag = slot.mark_draws[i];
            if (!tag.vs) continue;
            const uint64_t key = tag.vs * 31 + tag.ps;
            Sum& sum = sums[key];
            const double ns = ns_between(ticks[i], ticks[i + 1]);
            sum.ns += ns;
            ++sum.draws;
            sum.width = std::max(sum.width, tag.width);
            sum.height = std::max(sum.height, tag.height);
            pairs[key] = {tag.vs, tag.ps};
            total += ns;
          }
          std::vector<std::pair<double, uint64_t>> order;
          for (auto& [key, sum] : sums) order.emplace_back(sum.ns, key);
          std::sort(order.rbegin(), order.rend());
          std::string text;
          for (size_t i = 0; i < order.size() && i < 10; ++i) {
            const Sum& sum = sums[order[i].second];
            const auto& pair = pairs[order[i].second];
            text += fmt::format("\n    {:5.1f} ms  {:3} draws  VS {:016X} PS {:016X}  up to {}x{}",
                                sum.ns * 1e-6, sum.draws, pair.first, pair.second, sum.width,
                                sum.height);
          }
          REXGPU_INFO("[native] GPU profile of one frame: {:.1f} ms in {} draws{}", total * 1e-6,
                      slot.mark_draws.size(), text);
          // The resolves (copy only: until the next mark, which is the clears or the next draw).
          std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Sum> resolves;
          double resolve_total = 0.0;
          uint32_t resolve_count = 0;
          for (uint32_t i = 0; i + 1 < marks && i < slot.mark_draws.size(); ++i) {
            const FrameSlot::DrawTag& tag = slot.mark_draws[i];
            if (!(tag.ps & kResolveTag)) continue;
            Sum& sum = resolves[{uint32_t(tag.ps), tag.width, tag.height}];
            const double ns = ns_between(ticks[i], ticks[i + 1]);
            sum.ns += ns;
            ++sum.draws;
            resolve_total += ns;
            ++resolve_count;
          }
          std::vector<std::pair<double, std::tuple<uint32_t, uint32_t, uint32_t>>> resolve_order;
          for (auto& [key, sum] : resolves) resolve_order.emplace_back(sum.ns, key);
          std::sort(resolve_order.rbegin(), resolve_order.rend());
          std::string resolve_text;
          for (size_t i = 0; i < resolve_order.size() && i < 12; ++i) {
            const auto& [flags, w, h] = resolve_order[i].second;
            const Sum& sum = resolves[resolve_order[i].second];
            resolve_text += fmt::format("\n    {:5.2f} ms  {:3} x {}x{} {}{}{} msaa {}", sum.ns * 1e-6, sum.draws,
                                        w, h,
                                        (flags & 2) ? ((flags & 1) ? "copy depth" : "copy color") : "no copy",
                                        (flags & 4) ? ", clear color" : "", (flags & 8) ? ", clear depth" : "",
                                        flags >> 8);
          }
          REXGPU_INFO("[native] resolves of the same frame: {:.1f} ms in {}{}", resolve_total * 1e-6,
                      resolve_count, resolve_text);
        }
      }
    }
    slot.mark_kinds.clear();
    slot.mark_draws.clear();
    slot.profiled = false;
  }
  dfn.vkResetCommandPool(device->device(), slot.pool, 0);
  VkCommandBufferBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info);
  if (slot.timestamps) {
    dfn.vkCmdResetQueryPool(slot.command_buffer, slot.timestamps, 0, kMaxGpuMarks);
    cmd_write_timestamp_(slot.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
    slot.mark_kinds.push_back(0);
    slot.mark_draws.emplace_back();
    slot.profiled = profile_frame_;
  }
  gpu_kind_ = 0;
  record_start_ = std::chrono::steady_clock::now();
  return slot.command_buffer;
}

bool NativeCommandProcessor::SubmitSlot() {
  ui::vulkan::VulkanDevice* const device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  FrameSlot& slot = slots_[slot_index_];
  if (slot.timestamps && !slot.mark_kinds.empty()) {
    cmd_write_timestamp_(slot.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                         uint32_t(slot.mark_kinds.size()));
    slot.mark_kinds.push_back(0);
    slot.mark_draws.emplace_back();
  }
  dfn.vkEndCommandBuffer(slot.command_buffer);
  record_ns_ +=
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - record_start_)
          .count();
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

void NativeCommandProcessor::GpuMark(uint32_t kind) {
  FrameSlot& slot = slots_[slot_index_];
  if (!slot.profiled && !REXCVAR_GET(native_gpu_marks)) {
    return;
  }
  if (kind == gpu_kind_ || !frame_command_buffer_ || !slot.timestamps || slot.mark_kinds.empty() ||
      slot.mark_kinds.size() + 1 >= kMaxGpuMarks) {
    return;
  }
  // BOTTOM_OF_PIPE: the time the work recorded so far is done.
  cmd_write_timestamp_(frame_command_buffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                       uint32_t(slot.mark_kinds.size()));
  slot.mark_kinds.push_back(uint8_t(kind));
  slot.mark_draws.emplace_back();
  gpu_kind_ = kind;
}

void NativeCommandProcessor::GpuMarkDraw(uint64_t vs, uint64_t ps, uint32_t width, uint32_t height) {
  FrameSlot& slot = slots_[slot_index_];
  if (!slot.profiled || !frame_command_buffer_ || !slot.timestamps || slot.mark_kinds.empty() ||
      slot.mark_kinds.size() + 2 >= kMaxGpuMarks) {
    return;
  }
  cmd_write_timestamp_(frame_command_buffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                       uint32_t(slot.mark_kinds.size()));
  slot.mark_kinds.push_back(0);
  slot.mark_draws.push_back({vs, ps, width, height});
  gpu_kind_ = 0;
}

void NativeCommandProcessor::GpuMarkResolve(uint32_t width, uint32_t height, uint32_t flags) {
  FrameSlot& slot = slots_[slot_index_];
  if (!slot.profiled) {
    GpuMark(1);
    return;
  }
  if (!frame_command_buffer_ || !slot.timestamps || slot.mark_kinds.empty() ||
      slot.mark_kinds.size() + 2 >= kMaxGpuMarks) {
    return;
  }
  cmd_write_timestamp_(frame_command_buffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps,
                       uint32_t(slot.mark_kinds.size()));
  slot.mark_kinds.push_back(1);
  slot.mark_draws.push_back({0, kResolveTag | flags, width, height});
  gpu_kind_ = 1;
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
  if (trace_frame_) {
    const RegisterFile& regs = *register_file_;
    REXGPU_INFO("[native trace] copy after draw {}: control {:08X} color {:08X} depth {:08X} surface "
                "{:08X} window offset {:08X} dest {:08X} info {:08X} pitch {:08X} window scissor "
                "{:08X}-{:08X}",
                trace_draw_index_, regs[XE_GPU_REG_RB_COPY_CONTROL], regs[XE_GPU_REG_RB_COLOR_INFO],
                regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
                regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
                regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]);
  }
  targets_.Resolve(cb, *register_file_, *memory_);
  GpuMark(0);
  return true;
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  ++swaps_;
  profile_frame_ = profile_next_frame_;
  profile_next_frame_ = false;
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
          GpuMark(4);
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
    // The GPU timeline of the interval covers it all (busy or idle), so it must match the CPU clock.
    const double timeline = gpu_ns_ + gpu_gap_ns_;
    if (gpu_frames_ >= 10 && timeline > 0.0) {
      const double correction = std::clamp(seconds * 1e9 / timeline, 0.25, 4.0);
      gpu_ns_ *= correction;
      gpu_gap_ns_ *= correction;
      for (double& ns : gpu_kind_ns_) ns *= correction;
      timestamp_scale_ *= correction;
    }
    REXGPU_INFO("[native] {:.1f} swaps/s, {:.0f} draws ({:.0f} drawn) and {:.1f} copies per frame; "
                "{} guest shaders, {} translated, {} pipelines; {} texture uploads ({} MB); "
                "{} draws and {} resolves skipped in total; upload {} of {} MB (front buffer "
                "{:08X}); per frame: GPU {:.1f} ms (draws {:.1f}, resolve copies {:.1f}, clears "
                "{:.1f}, texture uploads {:.1f}, present {:.1f}; idle between frames {:.1f}), "
                "recording {:.1f} ms, waiting for a slot {:.1f} ms; GPU clock scale {:.3f}",
                swaps / seconds, swaps ? double(draws_ - report_draws_) / swaps : 0.0,
                swaps ? double(draws_done_ - report_draws_done_) / swaps : 0.0,
                swaps ? double(copies_ - report_copies_) / swaps : 0.0, shaders_.size(),
                shader_library_.loaded(), pipelines_.size(), textures_.uploads(),
                textures_.upload_bytes() >> 20, draws_skipped_, targets_.skipped(),
                upload_.used() >> 20, upload_.capacity() >> 20, frontbuffer_ptr,
                gpu_frames_ ? gpu_ns_ / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_kind_ns_[0] / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_kind_ns_[1] / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_kind_ns_[2] / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_kind_ns_[3] / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_kind_ns_[4] / gpu_frames_ * 1e-6 : 0.0,
                gpu_frames_ ? gpu_gap_ns_ / gpu_frames_ * 1e-6 : 0.0,
                swaps ? record_ns_ / swaps * 1e-6 : 0.0, swaps ? slot_wait_ns_ / swaps * 1e-6 : 0.0,
                timestamp_scale_);
    gpu_ns_ = record_ns_ = slot_wait_ns_ = gpu_gap_ns_ = 0.0;
    for (double& ns : gpu_kind_ns_) ns = 0.0;
    gpu_frames_ = 0;
    report_draws_done_ = draws_done_;
    profile_next_frame_ = cmd_write_timestamp_ != nullptr;
    report_start_ = now;
    report_draws_ = draws_;
    report_copies_ = copies_;
    report_swaps_ = swaps_;
  }
}

}  // namespace rex::graphics::native
