/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::vulkan {

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader_module_,
                                                     nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;
  // Analysis aid: with dump_shaders=<dir>, write every translated shader's
  // SPIR-V for the offline deko3d chain (SPIRV-Cross -> GLSL -> uam -> DKSH).
  const std::string& dump_shaders_dir = REXCVAR_GET(dump_shaders);
  if (!dump_shaders_dir.empty()) {
    static std::filesystem::path resolved_dump_dir;
    static bool dump_dir_resolved = false;
    if (!dump_dir_resolved) {
      dump_dir_resolved = true;
      std::error_code ec;
      // Horizon resolves bare absolute paths inconsistently through newlib;
      // try the configured path, a relative folder, then sdmc:/<path>.
      std::filesystem::path candidates[3];
      candidates[0] = dump_shaders_dir;
      candidates[1] = "shader_dump";
      if (!dump_shaders_dir.empty() && dump_shaders_dir.front() == '/') {
        candidates[2] = std::filesystem::path("sdmc:" + dump_shaders_dir);
      }
      for (const std::filesystem::path& candidate : candidates) {
        if (candidate.empty()) {
          continue;
        }
        std::filesystem::create_directories(candidate, ec);
        // Probe by writing: on Horizon, stat-based directory checks are
        // unreliable for these paths, while fopen works.
        std::ofstream probe(candidate / "dump_probe.tmp",
                            std::ios::binary | std::ios::trunc);
        if (probe) {
          probe.close();
          std::filesystem::remove(candidate / "dump_probe.tmp", ec);
          resolved_dump_dir = candidate;
          break;
        }
      }
      if (resolved_dump_dir.empty()) {
        REXLOG_ERROR("[shader-dump] cannot create any dump directory for '{}'",
                     dump_shaders_dir);
      } else {
        REXLOG_INFO("[shader-dump] writing SPIR-V to {}", resolved_dump_dir.string());
      }
    }
    if (!resolved_dump_dir.empty()) {
      char dump_name[80];
      std::snprintf(dump_name, sizeof(dump_name), "%s_%016llX_%016llX.spv",
                    shader().type() == xenos::ShaderType::kPixel ? "ps" : "vs",
                    static_cast<unsigned long long>(shader().ucode_data_hash()),
                    static_cast<unsigned long long>(modification()));
      std::filesystem::path dump_path = resolved_dump_dir / dump_name;
      std::ofstream dump_file(dump_path, std::ios::binary | std::ios::trunc);
      dump_file.write(reinterpret_cast<const char*>(translated_binary().data()),
                      std::streamsize(translated_binary().size()));
      if (!dump_file) {
        static int dump_failures = 0;
        if (dump_failures++ < 4) {
          REXLOG_ERROR("[shader-dump] failed to write {}", dump_path.string());
        }
      }
    }
  }
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = translated_binary().size();
  shader_module_create_info.pCode = reinterpret_cast<const uint32_t*>(translated_binary().data());
  if (vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                      &shader_module_create_info, nullptr,
                                                      &shader_module_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }
  return shader_module_;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type, const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords, const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

Shader::Translation* VulkanShader::CreateTranslationInstance(uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace rex::graphics::vulkan
