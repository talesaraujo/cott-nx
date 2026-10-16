/**
 * @file        native/shader_library.cpp
 * @brief       COTT: the translated shaders of the native renderer (see the header)
 */
#include <rex/graphics/native/shader_library.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::native {

namespace {

bool ReadFile(const std::string& path, std::vector<char>& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// Just enough JSON for the metadata sidecars: objects, arrays, integers, strings, booleans.
struct JsonValue {
  enum class Type { kNull, kNumber, kString, kBool, kArray, kObject } type = Type::kNull;
  int64_t number = 0;
  std::string string;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue> object;

  const JsonValue* Find(const char* key) const {
    auto it = object.find(key);
    return it != object.end() ? &it->second : nullptr;
  }
  int64_t Int(const char* key, int64_t fallback = 0) const {
    const JsonValue* value = Find(key);
    if (!value) return fallback;
    if (value->type == Type::kBool) return value->number;
    return value->type == Type::kNumber ? value->number : fallback;
  }
};

class JsonParser {
 public:
  JsonParser(const char* begin, const char* end) : p_(begin), end_(end) {}

  bool Parse(JsonValue& out) {
    if (!Value(out)) return false;
    Skip();
    return p_ == end_;
  }

 private:
  void Skip() {
    while (p_ < end_ && std::isspace(uint8_t(*p_))) ++p_;
  }
  bool Literal(const char* text) {
    const size_t length = std::strlen(text);
    if (size_t(end_ - p_) < length || std::memcmp(p_, text, length)) return false;
    p_ += length;
    return true;
  }
  bool String(std::string& out) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ == '\\' && p_ + 1 < end_) ++p_;
      out.push_back(*p_++);
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }
  bool Value(JsonValue& out) {
    Skip();
    if (p_ >= end_) return false;
    const char c = *p_;
    if (c == '{') {
      out.type = JsonValue::Type::kObject;
      ++p_;
      Skip();
      if (p_ < end_ && *p_ == '}') return ++p_, true;
      for (;;) {
        Skip();
        std::string key;
        if (!String(key)) return false;
        Skip();
        if (p_ >= end_ || *p_ != ':') return false;
        ++p_;
        if (!Value(out.object[key])) return false;
        Skip();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == '}') return ++p_, true;
        return false;
      }
    }
    if (c == '[') {
      out.type = JsonValue::Type::kArray;
      ++p_;
      Skip();
      if (p_ < end_ && *p_ == ']') return ++p_, true;
      for (;;) {
        out.array.emplace_back();
        if (!Value(out.array.back())) return false;
        Skip();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == ']') return ++p_, true;
        return false;
      }
    }
    if (c == '"') {
      out.type = JsonValue::Type::kString;
      return String(out.string);
    }
    if (Literal("true")) { out.type = JsonValue::Type::kBool; out.number = 1; return true; }
    if (Literal("false")) { out.type = JsonValue::Type::kBool; out.number = 0; return true; }
    if (Literal("null")) { out.type = JsonValue::Type::kNull; return true; }
    out.type = JsonValue::Type::kNumber;
    bool negative = false;
    if (*p_ == '-') { negative = true; ++p_; }
    if (p_ >= end_ || !std::isdigit(uint8_t(*p_))) return false;
    while (p_ < end_ && std::isdigit(uint8_t(*p_))) out.number = out.number * 10 + (*p_++ - '0');
    if (negative) out.number = -out.number;
    return true;
  }

  const char* p_;
  const char* end_;
};

// The color outputs of a fragment shader: Output variables decorated with a Location (SV_Target#);
// the depth output is a BuiltIn and has none.
uint32_t ScanColorOutputs(const std::vector<char>& spirv) {
  const auto* words = reinterpret_cast<const uint32_t*>(spirv.data());
  const size_t count = spirv.size() / 4;
  if (count < 5 || words[0] != 0x07230203u) {
    return 0;
  }
  std::map<uint32_t, uint32_t> locations;  // variable id -> location
  std::vector<uint32_t> outputs;
  for (size_t i = 5; i < count;) {
    const uint32_t opcode = words[i] & 0xFFFF;
    const uint32_t length = words[i] >> 16;
    if (!length || i + length > count) break;
    if (opcode == 71 /* OpDecorate */ && length >= 4 && words[i + 2] == 30 /* Location */) {
      locations[words[i + 1]] = words[i + 3];
    } else if (opcode == 59 /* OpVariable */ && length >= 4 && words[i + 3] == 3 /* Output */) {
      outputs.push_back(words[i + 2]);
    }
    i += length;
  }
  uint32_t mask = 0;
  for (uint32_t id : outputs) {
    auto it = locations.find(id);
    if (it != locations.end() && it->second < 32) {
      mask |= 1u << it->second;
    }
  }
  return mask;
}

}  // namespace

NativeShaderLibrary::~NativeShaderLibrary() {
  Shutdown();
}

bool NativeShaderLibrary::Initialize(ui::vulkan::VulkanDevice* device, const std::string& path) {
  device_ = device;
  path_ = path;
  if (!path_.empty() && path_.back() != '/') {
    path_.push_back('/');
  }
  REXGPU_INFO("[native] translated shaders from {}", path_.empty() ? "(native_shader_path not set)"
                                                                   : path_);
  return true;
}

void NativeShaderLibrary::Shutdown() {
  if (!device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  for (auto& map : shaders_) {
    for (auto& [hash, shader] : map) {
      if (shader && shader->module) {
        dfn.vkDestroyShaderModule(device_->device(), shader->module, nullptr);
      }
    }
    map.clear();
  }
  device_ = nullptr;
}

NativeShader* NativeShaderLibrary::Get(uint64_t hash, bool is_pixel) {
  auto& map = shaders_[is_pixel ? 1 : 0];
  auto it = map.find(hash);
  if (it != map.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<NativeShader>();
  shader->hash = hash;
  shader->is_pixel = is_pixel;
  if (!Load(*shader)) {
    shader.reset();
  }
  NativeShader* result = shader.get();
  map.emplace(hash, std::move(shader));
  return result;
}

bool NativeShaderLibrary::Load(NativeShader& shader) {
  const std::string stem =
      fmt::format("{}{}_{:016X}", path_, shader.is_pixel ? "ps" : "vs", shader.hash);
  std::vector<char> spirv, meta;
  if (!ReadFile(stem + ".spv", spirv) || spirv.size() < 20 || (spirv.size() & 3)) {
    REXGPU_WARN("[native] missing translated shader {}.spv: its draws are skipped", stem);
    return false;
  }
  JsonValue json;
  if (!ReadFile(stem + ".meta.json", meta) ||
      !JsonParser(meta.data(), meta.data() + meta.size()).Parse(json) ||
      json.type != JsonValue::Type::kObject) {
    REXGPU_WARN("[native] missing or unreadable {}.meta.json: its draws are skipped", stem);
    return false;
  }

  if (const JsonValue* attributes = json.Find("attributes")) {
    for (const JsonValue& a : attributes->array) {
      // Attributes the translator could not express as vertex input (indirect fetches, or ones read
      // from a buffer in the shader) are not supported: none of the game's shaders has them.
      if (a.Int("location", -1) < 0 || a.Int("indirect") || a.Int("bufferRead")) {
        REXGPU_WARN("[native] {}: attribute without a plain vertex input, ignored", stem);
        continue;
      }
      NativeVertexAttribute attribute;
      attribute.location = uint32_t(a.Int("location"));
      attribute.fetch_slot = uint32_t(a.Int("fetchSlot"));
      attribute.format = uint32_t(a.Int("format"));
      attribute.is_signed = a.Int("signed") != 0;
      attribute.is_integer = a.Int("integer") != 0;
      attribute.stride_dwords = uint32_t(a.Int("strideDwords"));
      attribute.offset_dwords = uint32_t(a.Int("offsetDwords"));
      shader.attributes.push_back(attribute);
    }
  }
  if (const JsonValue* slots = json.Find("tfetchConsts")) {
    for (const JsonValue& v : slots->array) shader.texture_slots.push_back(uint32_t(v.number));
  }
  if (const JsonValue* dims = json.Find("tfetchDims")) {
    for (const JsonValue& v : dims->array) shader.texture_dimensions.push_back(uint32_t(v.number));
  }
  const JsonValue* alu = json.Find("aluConsts");
  shader.uses_alu_constants = (alu && !alu->array.empty()) || json.Int("aluDynamic");
  if (shader.is_pixel) {
    shader.color_output_mask = ScanColorOutputs(spirv);
  }

  VkShaderModuleCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spirv.size();
  info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
  if (device_->functions().vkCreateShaderModule(device_->device(), &info, nullptr,
                                                &shader.module) != VK_SUCCESS) {
    REXGPU_ERROR("[native] vkCreateShaderModule failed for {}", stem);
    return false;
  }
  REXGPU_DEBUG("[native] shader {}: {} attributes, {} textures, outputs {:X}", stem,
               shader.attributes.size(), shader.texture_slots.size(), shader.color_output_mask);
  return true;
}

}  // namespace rex::graphics::native
