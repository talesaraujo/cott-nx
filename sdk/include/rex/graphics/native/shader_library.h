/**
 * @file        native/shader_library.h
 * @brief       COTT: the translated shaders of the native renderer (Phase 3, milestone M3c)
 *
 * The game's 31 Xenos shaders are translated ahead of time on the PC (tools/shaders: MojoRecomp's
 * patched XenosRecomp plus DXC) into SPIR-V, one `vs_<HASH>.spv` / `ps_<HASH>.spv` per microcode, each
 * with a `.meta.json` sidecar. HASH is the XXH3 of the guest microcode bytes, the key LoadShader computes.
 * The files are generated from the disc and are never committed; the library reads them from the folder
 * named by the `native_shader_path` cvar.
 *
 * Shader ABI (MojoRecomp's patch of XenosRecomp):
 *  - push constants: three buffer device addresses (VS constants, PS constants, shared constants);
 *  - textures: four descriptor sets of 16 entries (2D, 3D, cube images, samplers), indexed by the fetch
 *    constant slot through a table at the start of the shared constants;
 *  - specialization constant 0: alpha test (bit 1 enable, bits 8..10 compare function);
 *  - vertex attributes: one per fetch, described by the VS metadata.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

struct NativeVertexAttribute {
  uint32_t location = 0;
  uint32_t fetch_slot = 0;
  uint32_t format = 0;  // xenos::VertexFormat
  bool is_signed = false;
  bool is_integer = false;
  uint32_t stride_dwords = 0;
  uint32_t offset_dwords = 0;
};

struct NativeShader {
  uint64_t hash = 0;
  bool is_pixel = false;
  VkShaderModule module = VK_NULL_HANDLE;
  std::vector<NativeVertexAttribute> attributes;
  std::vector<uint32_t> texture_slots;
  std::vector<uint32_t> texture_dimensions;
  bool uses_alu_constants = false;
  // Pixel shaders: the color outputs written (bit per render target).
  uint32_t color_output_mask = 0;
};

class NativeShaderLibrary {
 public:
  NativeShaderLibrary() = default;
  ~NativeShaderLibrary();

  bool Initialize(ui::vulkan::VulkanDevice* device, const std::string& path);
  void Shutdown();

  // The translated shader, loaded on first use; null (logged once) when it is missing.
  NativeShader* Get(uint64_t hash, bool is_pixel);

  size_t loaded() const { return shaders_[0].size() + shaders_[1].size(); }

 private:
  bool Load(NativeShader& shader);

  ui::vulkan::VulkanDevice* device_ = nullptr;
  std::string path_;
  // By microcode hash, vertex shaders [0] and pixel shaders [1]. Missing files are kept as null.
  std::unordered_map<uint64_t, std::unique_ptr<NativeShader>> shaders_[2];
};

}  // namespace rex::graphics::native
