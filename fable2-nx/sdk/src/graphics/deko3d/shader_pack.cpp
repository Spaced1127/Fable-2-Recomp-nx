/**
 * @file        graphics/deko3d/shader_pack.cpp
 * @brief       Runtime loader for the pre-compiled DKSH shader pack.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>
#include <fstream>

#include <rex/graphics/deko3d/shader_pack.h>
#include <rex/logging.h>

namespace rex::graphics::deko3d {

namespace {
constexpr char kMagic[8] = {'F', '2', 'D', 'K', 'S', 'H', '2', '\0'};
// u32 name length, name, u32 binding count, 12 bytes per binding, u32 offset,
// u32 size.
constexpr size_t kBindingRecordSize = 12;

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t pos) {
  uint32_t value = 0;
  std::memcpy(&value, bytes.data() + pos, sizeof(value));
  return value;
}

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t pos) {
  uint16_t value = 0;
  std::memcpy(&value, bytes.data() + pos, sizeof(value));
  return value;
}

}  // namespace

bool Deko3DShaderPack::Load(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
    REXGPU_ERROR(
        "deko3d: {} is not a F2DKSH2 shader pack; regenerate it with "
        "tools/deko3d/convert_shaders.py and pack_shaders.py",
        path.string());
    return false;
  }
  const uint32_t count = ReadU32(bytes, 8);
  size_t pos = 12;
  uint32_t bindings_total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > bytes.size()) {
      break;
    }
    const uint32_t name_len = ReadU32(bytes, pos);
    pos += 4;
    if (pos + name_len + 4 > bytes.size()) {
      break;
    }
    std::string name(reinterpret_cast<const char*>(bytes.data() + pos), name_len);
    pos += name_len;
    const uint32_t binding_count = ReadU32(bytes, pos);
    pos += 4;
    if (pos + size_t(binding_count) * kBindingRecordSize + 8 > bytes.size()) {
      break;
    }
    Entry entry;
    entry.bindings_offset = uint32_t(bindings_.size());
    entry.bindings_count = binding_count;
    for (uint32_t j = 0; j < binding_count; ++j) {
      Deko3DTextureBinding binding;
      binding.binding = ReadU16(bytes, pos);
      binding.texture_fetch_constant = bytes[pos + 2];
      binding.sampler_fetch_constant = bytes[pos + 3];
      binding.dimension = bytes[pos + 4];
      binding.is_signed = bytes[pos + 5];
      binding.mag_filter = xenos::TextureFilter(bytes[pos + 6]);
      binding.min_filter = xenos::TextureFilter(bytes[pos + 7]);
      binding.mip_filter = xenos::TextureFilter(bytes[pos + 8]);
      binding.aniso_filter = xenos::AnisoFilter(bytes[pos + 9]);
      pos += kBindingRecordSize;
      bindings_.push_back(binding);
    }
    bindings_total += binding_count;
    entry.data_offset = ReadU32(bytes, pos);
    entry.data_size = ReadU32(bytes, pos + 4);
    pos += 8;
    if (uint64_t(entry.data_offset) + entry.data_size <= bytes.size()) {
      entries_.emplace(std::move(name), entry);
    }
  }
  if (entries_.empty()) {
    bindings_.clear();
    return false;
  }
  data_ = std::move(bytes);
  REXGPU_INFO("deko3d: shader pack {} loaded ({} shaders, {} texture bindings)",
              path.string(), entries_.size(), bindings_total);
  return true;
}

const uint8_t* Deko3DShaderPack::FindPrefix(const std::string& prefix, uint32_t& size_out,
                                            const Deko3DTextureBinding** bindings_out,
                                            uint32_t* bindings_count_out,
                                            std::string* key_out) const {
  for (const auto& entry : entries_) {
    if (entry.first.compare(0, prefix.size(), prefix) == 0) {
      size_out = entry.second.data_size;
      if (bindings_out) {
        *bindings_out = Bindings(entry.second);
      }
      if (bindings_count_out) {
        *bindings_count_out = entry.second.bindings_count;
      }
      if (key_out) {
        *key_out = entry.first;
      }
      return data_.data() + entry.second.data_offset;
    }
  }
  size_out = 0;
  if (bindings_out) {
    *bindings_out = nullptr;
  }
  if (bindings_count_out) {
    *bindings_count_out = 0;
  }
  return nullptr;
}

}  // namespace rex::graphics::deko3d
