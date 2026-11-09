/**
 * @file        graphics/deko3d/shader.h
 * @brief       Guest shaders as pre-compiled DKSH, and the code arena they
 *              live in.
 *
 * The deko3d backend does not translate Xenos microcode at runtime. Shaders
 * are converted on the host (tools/deko3d/convert_shaders.py) and shipped in
 * Game_files/native_shaders/shaders.dk.pak, keyed by
 * "<ps|vs>_<ucode hash>_<translator modification>" - the names the Vulkan
 * backend's dump_shaders aid writes. A shader whose DKSH is not in the pack
 * cannot be drawn, so the miss is reported with the exact key and the draw is
 * skipped.
 *
 * The modification is per draw, not per shader: the same microcode is
 * recompiled for each interpolator mask, user clip plane count, host vertex
 * shader type and depth/stencil mode it is used with. So one Deko3DShader
 * holds the microcode (for the analysis the constant upload and the
 * modification both need) and a cache of the DKSH variants selected so far.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <deko3d.hpp>

#include <rex/graphics/deko3d/shader_pack.h>
#include <rex/graphics/pipeline/shader/shader.h>

namespace rex::ui::deko3d {
class Deko3DDevice;
}

namespace rex::graphics::deko3d {

// Growable DkMemBlockFlags_Code arena. deko3d reads a shader's control section
// and code straight out of the block it was initialized from, so every DKSH
// must stay resident for the lifetime of its dk::Shader. Blobs are never
// freed: the set of shaders a title uses is bounded, and the guest reloads
// them constantly.
class Deko3DShaderCodeArena {
 public:
  explicit Deko3DShaderCodeArena(ui::deko3d::Deko3DDevice& device);
  ~Deko3DShaderCodeArena();

  Deko3DShaderCodeArena(const Deko3DShaderCodeArena&) = delete;
  Deko3DShaderCodeArena& operator=(const Deko3DShaderCodeArena&) = delete;

  // Copies a whole .dksh file into the arena and initializes shader_out from
  // it. False if the DKSH is malformed, does not fit, or needs more per-warp
  // scratch than the queue was created with.
  bool Load(const uint8_t* dksh, size_t size, dk::Shader& shader_out);

  // The queue's per-warp scratch memory, which every loaded shader must fit
  // in. Set from the queue maker so the check and the allocation agree.
  uint32_t per_warp_scratch_size() const { return per_warp_scratch_size_; }
  void set_per_warp_scratch_size(uint32_t size) { per_warp_scratch_size_ = size; }

  size_t bytes_used() const { return bytes_used_; }
  size_t block_count() const { return blocks_.size(); }

 private:
  ui::deko3d::Deko3DDevice& device_;
  // Block and the first unused byte in it.
  std::vector<std::pair<dk::UniqueMemBlock, uint32_t>> blocks_;
  uint32_t per_warp_scratch_size_ = 0;
  size_t bytes_used_ = 0;
};

class Deko3DShader final : public Shader {
 public:
  // One DKSH variant of this shader's microcode, loaded on first use.
  struct HostShader {
    dk::Shader shader{};
    // Combined texture+sampler bindings, in deko3d texture slot order.
    std::vector<Deko3DTextureBinding> texture_bindings;
    // One bit per texture fetch constant the DKSH actually samples.
    uint32_t used_texture_mask = 0;
    // False when the exact modification was missing and a different variant of
    // the same microcode was used instead (the draw is likely wrong, but
    // something is on screen and the log says which key to add).
    bool exact_modification = false;
    std::string pack_key;
  };

  Deko3DShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
               const uint32_t* ucode_dwords, size_t ucode_dword_count,
               const Deko3DShaderPack* pack);
  ~Deko3DShader() override;

  // The DKSH for this draw's translator modification, loaded on first use.
  // Returns nullptr when the pack has no variant of this microcode at all, or
  // when the DKSH failed to load; either way the failure is remembered, so a
  // missing shader is reported once rather than on every draw.
  const HostShader* GetHostShader(uint64_t modification, Deko3DShaderCodeArena& arena);

  // Prefix the pack keys of this shader start with: "<ps|vs>_<ucode hash>_".
  const std::string& pack_prefix() const { return pack_prefix_; }

 private:
  const HostShader* LoadHostShader(const std::string& key, uint64_t modification, bool exact,
                                   Deko3DShaderCodeArena& arena);

  const Deko3DShaderPack* pack_;
  std::string pack_prefix_;
  // Keyed by modification; pointers stay valid because the values are held in
  // unique_ptrs. A null value marks a modification that failed to load.
  std::unordered_map<uint64_t, std::unique_ptr<HostShader>> host_shaders_;
};

}  // namespace rex::graphics::deko3d
