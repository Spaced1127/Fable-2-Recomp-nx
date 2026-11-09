/**
 * @file        graphics/deko3d/shader_pack.h
 * @brief       Runtime loader for the pre-compiled DKSH shader pack.
 *
 * The pack is generated on the host from the user's own game shaders
 * (tools/deko3d/convert_shaders.py + pack_shaders.py) and is never committed.
 * Keys are the dump names: "ps_<ucode_hash>_<modification>" / "vs_...".
 *
 * Besides the DKSH bytes each entry carries the texture/sampler binding table
 * the converter produced. The backend cannot recover it from the DKSH (uam
 * drops the SPIR-V names), and it cannot re-derive it either: the Vulkan
 * backend numbers texture and sampler descriptors separately per stage, while
 * deko3d has one texture namespace per stage holding combined image+sampler
 * handles.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::deko3d {

// One combined texture+sampler binding of a packed shader, at the deko3d
// texture slot the converter gave it.
struct Deko3DTextureBinding {
  uint16_t binding = 0;
  uint8_t texture_fetch_constant = 0;
  uint8_t sampler_fetch_constant = 0;
  // 0 = 2D array, 1 = cube, 2 = 3D. Matches xenos::FetchOpDimension order
  // used by the SPIR-V translator's image types (2D is always an array).
  uint8_t dimension = 0;
  uint8_t is_signed = 0;
  xenos::TextureFilter mag_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::TextureFilter min_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::TextureFilter mip_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kUseFetchConst;
};

class Deko3DShaderPack {
 public:
  struct Entry {
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t bindings_offset = 0;
    uint32_t bindings_count = 0;
  };

  bool Load(const std::filesystem::path& path);

  // Returns the DKSH bytes for a key, or nullptr. The pointer stays valid for
  // the lifetime of the pack.
  const uint8_t* Find(const std::string& key, uint32_t& size_out) const;

  // Finds any entry whose key starts with prefix (used to match a shader hash
  // regardless of the translator modification bits). Returns nullptr if none.
  const uint8_t* FindPrefix(const std::string& prefix, uint32_t& size_out,
                            const Deko3DTextureBinding** bindings_out = nullptr,
                            uint32_t* bindings_count_out = nullptr,
                            std::string* key_out = nullptr) const;

  size_t count() const { return entries_.size(); }
  bool loaded() const { return !entries_.empty(); }

 private:
  const Deko3DTextureBinding* Bindings(const Entry& entry) const {
    return entry.bindings_count ? bindings_.data() + entry.bindings_offset : nullptr;
  }

  std::vector<uint8_t> data_;
  std::vector<Deko3DTextureBinding> bindings_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace rex::graphics::deko3d
