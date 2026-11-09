/**
 * @file        graphics/deko3d/shader_pack.h
 * @brief       Runtime loader for the pre-compiled DKSH shader pack.
 *
 * The pack is generated on the host from the user's own game shaders
 * (tools/deko3d/convert_shaders.py + pack_shaders.py) and is never committed.
 * Keys are the dump names: "ps_<ucode_hash>_<modification>" / "vs_...".
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

namespace rex::graphics::deko3d {

class Deko3DShaderPack {
 public:
  bool Load(const std::filesystem::path& path);

  // Returns the DKSH bytes for a key, or nullptr. The pointer stays valid for
  // the lifetime of the pack.
  const uint8_t* Find(const std::string& key, uint32_t& size_out) const;

  size_t count() const { return entries_.size(); }
  bool loaded() const { return !entries_.empty(); }

 private:
  std::vector<uint8_t> data_;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> entries_;
};

}  // namespace rex::graphics::deko3d
