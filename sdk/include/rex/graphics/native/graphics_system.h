/**
 * @file        native/graphics_system.h
 * @brief       COTT: the native renderer's graphics system (Phase 3), chosen with gpu_backend=native
 *
 * Instead of imitating the Xenos GPU (EDRAM, fragment shader interlock, shaders translated while playing),
 * the native renderer will draw the game's frames with Vulkan directly, from a SPIR-V library built on the
 * PC (tools/shaders). It reuses everything the SDK already does with the game's command ring: the PM4
 * parser, the register file, interrupts, ring pointer write-back and GPU writes to memory
 * (CommandProcessor). Only the draws, copies and swaps are its own.
 *
 * Milestone M2 (this version): the skeleton. Draws and copies are counted and dropped, so the game runs
 * with a black screen and without any GPU work. That also measures the ceiling the CPU side allows.
 */
#pragma once

#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>

namespace rex::graphics::native {

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::native
