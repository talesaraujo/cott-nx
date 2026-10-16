/**
 * @file        native/graphics_system.cpp
 * @brief       COTT: the native renderer's graphics system (see the header)
 */
#include <rex/graphics/native/command_processor.h>
#include <rex/graphics/native/graphics_system.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::native {

NativeGraphicsSystem::NativeGraphicsSystem() = default;
NativeGraphicsSystem::~NativeGraphicsSystem() = default;

std::string NativeGraphicsSystem::name() const {
  return "Native";
}

// The same Vulkan provider and presenter as the emulation: window, swapchain and the debug overlay work
// the same way.
void NativeGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::native
