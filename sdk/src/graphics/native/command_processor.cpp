/**
 * @file        native/command_processor.cpp
 * @brief       COTT: the native renderer's command processor, milestone M2 (see native/graphics_system.h)
 */
#include <rex/graphics/native/command_processor.h>

#include <xxhash.h>

#include <rex/graphics/flags.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/platform.h>

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
  report_start_ = std::chrono::steady_clock::now();
  REXGPU_INFO("[native] native renderer, milestone M2: draws and copies are dropped (black screen)");
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
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

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType /*prim_type*/, uint32_t /*index_count*/,
                                       IndexBufferInfo* /*index_buffer_info*/,
                                       bool /*major_mode_explicit*/) {
  // A resolve (EDRAM to memory copy) arrives as a draw with RB_MODECONTROL in copy mode, as in the
  // emulation (VulkanCommandProcessor::IssueDraw).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++draws_;
  return true;
}

bool NativeCommandProcessor::IssueCopy() {
  ++copies_;
  return true;
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  ++swaps_;
  const auto now = std::chrono::steady_clock::now();
  if (now - report_start_ >= kReportInterval) {
    const double seconds = std::chrono::duration<double>(now - report_start_).count();
    const uint64_t swaps = swaps_ - report_swaps_;
    REXGPU_INFO("[native] {:.1f} swaps/s, {:.0f} draws and {:.1f} copies per frame, {} shaders "
                "(front buffer {:08X} {}x{})",
                swaps / seconds, swaps ? double(draws_ - report_draws_) / swaps : 0.0,
                swaps ? double(copies_ - report_copies_) / swaps : 0.0, shaders_.size(),
                frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
    report_start_ = now;
    report_draws_ = draws_;
    report_copies_ = copies_;
    report_swaps_ = swaps_;
  }
}

}  // namespace rex::graphics::native
