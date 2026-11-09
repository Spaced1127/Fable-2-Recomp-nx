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
constexpr char kMagic[8] = {'F', '2', 'D', 'K', 'S', 'H', '1', '\0'};
}

bool Deko3DShaderPack::Load(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
    REXGPU_ERROR("deko3d: {} is not a F2DKSH1 shader pack", path.string());
    return false;
  }
  uint32_t count = 0;
  std::memcpy(&count, bytes.data() + 8, 4);
  size_t pos = 12;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > bytes.size()) {
      break;
    }
    uint32_t name_len = 0;
    std::memcpy(&name_len, bytes.data() + pos, 4);
    pos += 4;
    if (pos + name_len + 8 > bytes.size()) {
      break;
    }
    std::string name(reinterpret_cast<const char*>(bytes.data() + pos), name_len);
    pos += name_len;
    uint32_t offset = 0;
    uint32_t size = 0;
    std::memcpy(&offset, bytes.data() + pos, 4);
    std::memcpy(&size, bytes.data() + pos + 4, 4);
    pos += 8;
    if (offset + size <= bytes.size()) {
      entries_.emplace(std::move(name), std::make_pair(offset, size));
    }
  }
  if (entries_.empty()) {
    return false;
  }
  data_ = std::move(bytes);
  REXGPU_INFO("deko3d: shader pack {} loaded ({} shaders)", path.string(),
              entries_.size());
  return true;
}

const uint8_t* Deko3DShaderPack::Find(const std::string& key, uint32_t& size_out) const {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    size_out = 0;
    return nullptr;
  }
  size_out = it->second.second;
  return data_.data() + it->second.first;
}

const uint8_t* Deko3DShaderPack::FindPrefix(const std::string& prefix, uint32_t& size_out,
                                            std::string* key_out) const {
  for (const auto& entry : entries_) {
    if (entry.first.compare(0, prefix.size(), prefix) == 0) {
      size_out = entry.second.second;
      if (key_out) {
        *key_out = entry.first;
      }
      return data_.data() + entry.second.first;
    }
  }
  size_out = 0;
  return nullptr;
}

}  // namespace rex::graphics::deko3d
