/**
 * @file        graphics/deko3d/shader.cpp
 * @brief       DKSH code arena and the guest shader wrapper.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>
#include <cstring>

#include <rex/graphics/deko3d/shader.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

namespace {

// uam's .dksh file format. libnx ships the loader in deko3d but not these
// headers, and the only field the backend needs is the per-warp scratch
// requirement, which must not exceed what the queue was created with (a
// shader needing more fails at draw time with no diagnostic).
constexpr uint32_t kDkshMagic = 0x48534B44;  // "DKSH"
constexpr size_t kDkshHeaderSize = 24;
constexpr size_t kDkshProgramsOffsetField = 16;
constexpr size_t kDkshPerWarpScratchOffset = 20;

// deko3d aligns shader code to 256 bytes and reserves the last 1 KiB of every
// code block.
constexpr uint32_t kShaderCodeAlignment = 0x100;
constexpr uint32_t kShaderCodeUnusableSize = 0x400;
constexpr uint32_t kShaderCodeBlockSize = 4 << 20;

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t ReadU32(const uint8_t* bytes, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

}  // namespace

Deko3DShaderCodeArena::Deko3DShaderCodeArena(ui::deko3d::Deko3DDevice& device)
    : device_(device) {}

Deko3DShaderCodeArena::~Deko3DShaderCodeArena() {
  // The dk::Shader objects referencing these blocks are owned by the command
  // processor's shader map, which is destroyed first.
  blocks_.clear();
}

bool Deko3DShaderCodeArena::Load(const uint8_t* dksh, size_t size, dk::Shader& shader_out) {
  if (!dksh || size < kDkshHeaderSize || size > UINT32_MAX) {
    return false;
  }
  if (ReadU32(dksh, 0) != kDkshMagic) {
    REXGPU_ERROR("deko3d: shader pack entry is not a DKSH file");
    return false;
  }
  const uint32_t programs_offset = ReadU32(dksh, kDkshProgramsOffsetField);
  if (programs_offset + kDkshPerWarpScratchOffset + 4 > size) {
    REXGPU_ERROR("deko3d: DKSH program header is out of range ({} bytes)", size);
    return false;
  }
  const uint32_t per_warp_scratch = ReadU32(dksh, programs_offset + kDkshPerWarpScratchOffset);
  if (per_warp_scratch_size_ && per_warp_scratch > per_warp_scratch_size_) {
    REXGPU_ERROR(
        "deko3d: shader needs {} bytes of per-warp scratch, the queue has {}; not loaded",
        per_warp_scratch, per_warp_scratch_size_);
    return false;
  }

  const uint32_t blob_size = uint32_t(size);
  if (blocks_.empty() ||
      AlignUp(blocks_.back().second, kShaderCodeAlignment) + blob_size >
          kShaderCodeBlockSize - kShaderCodeUnusableSize) {
    if (blob_size > kShaderCodeBlockSize - kShaderCodeUnusableSize) {
      REXGPU_ERROR("deko3d: {} bytes of DKSH do not fit a code block", blob_size);
      return false;
    }
    dk::UniqueMemBlock block =
        dk::MemBlockMaker{device_.device(), kShaderCodeBlockSize}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                      DkMemBlockFlags_Code)
            .create();
    if (!block) {
      REXGPU_ERROR("deko3d: shader code block allocation failed");
      return false;
    }
    blocks_.emplace_back(std::move(block), 0);
    REXGPU_INFO("deko3d: shader code block {} ({} MiB, {} KiB used so far)", blocks_.size(),
                kShaderCodeBlockSize >> 20, bytes_used_ >> 10);
  }

  auto& block_entry = blocks_.back();
  const uint32_t offset = AlignUp(block_entry.second, kShaderCodeAlignment);
  std::memcpy(static_cast<uint8_t*>(block_entry.first.getCpuAddr()) + offset, dksh, blob_size);
  dk::ShaderMaker{block_entry.first, offset}.initialize(shader_out);
  if (!shader_out.isValid()) {
    REXGPU_ERROR("deko3d: {} bytes of DKSH at offset {} are not valid", blob_size, offset);
    return false;
  }
  block_entry.second = offset + blob_size;
  bytes_used_ += blob_size;
  return true;
}

Deko3DShader::Deko3DShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                           const uint32_t* ucode_dwords, size_t ucode_dword_count,
                           const Deko3DShaderPack* pack)
    : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count), pack_(pack) {
  char prefix[32];
  std::snprintf(prefix, sizeof(prefix), "%s_%016llX_",
                shader_type == xenos::ShaderType::kPixel ? "ps" : "vs",
                static_cast<unsigned long long>(ucode_data_hash));
  pack_prefix_ = prefix;
}

Deko3DShader::~Deko3DShader() = default;

const Deko3DShader::HostShader* Deko3DShader::GetHostShader(uint64_t modification,
                                                            Deko3DShaderCodeArena& arena) {
  auto cached = host_shaders_.find(modification);
  if (cached != host_shaders_.end()) {
    // A null entry is a variant that failed to load; don't retry it.
    return cached->second.get();
  }
  if (!pack_) {
    host_shaders_.emplace(modification, nullptr);
    return nullptr;
  }

  char key[64];
  std::snprintf(key, sizeof(key), "%s%016llX", pack_prefix_.c_str(),
                static_cast<unsigned long long>(modification));
  const HostShader* loaded = LoadHostShader(key, modification, true, arena);
  if (loaded) {
    return loaded;
  }

  // No variant for this exact modification. Fall back to any other variant of
  // the same microcode so something is drawn, and say which key is missing -
  // the interpolator mask and depth/stencil mode parts only change which
  // interpolators and depth paths exist, so the wrong variant is usually close
  // enough to tell whether the rest of the draw path works.
  uint32_t fallback_size = 0;
  const Deko3DTextureBinding* fallback_bindings = nullptr;
  uint32_t fallback_binding_count = 0;
  std::string fallback_key;
  const uint8_t* fallback_dksh = pack_->FindPrefix(pack_prefix_, fallback_size, &fallback_bindings,
                                                   &fallback_binding_count, &fallback_key);
  if (fallback_dksh) {
    REXGPU_WARN(
        "deko3d: {} is not in the pack; using {} instead (re-dump with dump_shaders "
        "to add it)",
        key, fallback_key);
    return LoadHostShader(fallback_key, modification, false, arena);
  }
  static bool logged_missing_shader = false;
  if (!logged_missing_shader) {
    logged_missing_shader = true;
    REXGPU_WARN(
        "deko3d: shader {} is not in the pack at all; draws using it are skipped",
        pack_prefix_);
  }
  host_shaders_.emplace(modification, nullptr);
  return nullptr;
}

const Deko3DShader::HostShader* Deko3DShader::LoadHostShader(const std::string& key,
                                                             uint64_t modification, bool exact,
                                                             Deko3DShaderCodeArena& arena) {
  uint32_t dksh_size = 0;
  const Deko3DTextureBinding* bindings = nullptr;
  uint32_t binding_count = 0;
  // A full key is its own prefix, so this serves both the exact lookup and the
  // fallback (whose key FindPrefix already resolved), and returns the binding
  // table alongside the DKSH bytes.
  const uint8_t* dksh = pack_->FindPrefix(key, dksh_size, &bindings, &binding_count);
  if (!dksh || !dksh_size) {
    return nullptr;
  }
  std::unique_ptr<HostShader> host_shader = std::make_unique<HostShader>();
  if (!arena.Load(dksh, dksh_size, host_shader->shader)) {
    host_shaders_.emplace(modification, nullptr);
    return nullptr;
  }
  if (bindings && binding_count) {
    host_shader->texture_bindings.assign(bindings, bindings + binding_count);
    for (const Deko3DTextureBinding& binding : host_shader->texture_bindings) {
      if (binding.texture_fetch_constant < 32) {
        host_shader->used_texture_mask |= UINT32_C(1) << binding.texture_fetch_constant;
      }
    }
  }
  host_shader->exact_modification = exact;
  host_shader->pack_key = key;
  HostShader* result = host_shader.get();
  host_shaders_.emplace(modification, std::move(host_shader));
  return result;
}

}  // namespace rex::graphics::deko3d
