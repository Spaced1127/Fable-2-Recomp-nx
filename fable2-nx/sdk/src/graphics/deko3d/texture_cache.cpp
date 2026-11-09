/**
 * @file        graphics/deko3d/texture_cache.cpp
 * @brief       Guest textures as deko3d images (D2 bring-up).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstring>
#include <mutex>

#include <rex/graphics/deko3d/ring_buffer.h>
#include <rex/graphics/deko3d/shared_memory.h>
#include <rex/graphics/deko3d/texture_cache.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>
#include <rex/ui/deko3d/deko3d_device.h>

namespace rex::graphics::deko3d {

namespace {

constexpr uint32_t kUploadCommandSize = 64 << 10;

bool GetDekoImageFormat(xenos::TextureFormat format, DkImageFormat& format_out) {
  switch (GetBaseFormat(format)) {
    case xenos::TextureFormat::k_8:
      format_out = DkImageFormat_R8_Unorm;
      return true;
    case xenos::TextureFormat::k_8_8:
      format_out = DkImageFormat_RG8_Unorm;
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
      format_out = DkImageFormat_RGBA8_Unorm;
      return true;
    case xenos::TextureFormat::k_4_4_4_4:
      format_out = DkImageFormat_RGBA4_Unorm;
      return true;
    case xenos::TextureFormat::k_1_5_5_5:
      format_out = DkImageFormat_RGB5A1_Unorm;
      return true;
    case xenos::TextureFormat::k_5_6_5:
      format_out = DkImageFormat_RGB565_Unorm;
      return true;
    case xenos::TextureFormat::k_2_10_10_10:
      format_out = DkImageFormat_RGB10A2_Unorm;
      return true;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      format_out = DkImageFormat_RG11B10_Float;
      return true;
    case xenos::TextureFormat::k_16:
      format_out = DkImageFormat_R16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_16:
      format_out = DkImageFormat_RG16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_16_16_16:
      format_out = DkImageFormat_RGBA16_Unorm;
      return true;
    case xenos::TextureFormat::k_16_FLOAT:
      format_out = DkImageFormat_R16_Float;
      return true;
    case xenos::TextureFormat::k_16_16_FLOAT:
      format_out = DkImageFormat_RG16_Float;
      return true;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      format_out = DkImageFormat_RGBA16_Float;
      return true;
    case xenos::TextureFormat::k_32:
      format_out = DkImageFormat_R32_Float;
      return true;
    case xenos::TextureFormat::k_32_32:
      format_out = DkImageFormat_RG32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_32:
      format_out = DkImageFormat_RGBA32_Float;
      return true;
    case xenos::TextureFormat::k_32_FLOAT:
      format_out = DkImageFormat_R32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_FLOAT:
      format_out = DkImageFormat_RG32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_FLOAT:
      format_out = DkImageFormat_RGB32_Float;
      return true;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      format_out = DkImageFormat_RGBA32_Float;
      return true;
    case xenos::TextureFormat::k_DXT1:
      format_out = DkImageFormat_RGBA_BC1;
      return true;
    case xenos::TextureFormat::k_DXT2_3:
      format_out = DkImageFormat_RGBA_BC2;
      return true;
    case xenos::TextureFormat::k_DXT4_5:
      format_out = DkImageFormat_RGBA_BC3;
      return true;
    case xenos::TextureFormat::k_DXN:
      format_out = DkImageFormat_RG_BC5_Unorm;
      return true;
    case xenos::TextureFormat::k_24_8:
      format_out = DkImageFormat_Z24S8;
      return true;
    case xenos::TextureFormat::k_24_8_FLOAT:
      format_out = DkImageFormat_ZF32_X24S8;
      return true;
    default:
      return false;
  }
}

}  // namespace

class Deko3DTextureCache::TextureImpl final : public Texture {
 public:
  TextureImpl(Deko3DTextureCache& texture_cache, TextureKey key)
      : Texture(texture_cache, key), texture_cache_(texture_cache) {}

  bool Initialize(DkImageFormat format, DkImageType type, uint32_t width, uint32_t height,
                  uint32_t depth) {
    width_ = width;
    height_ = height;
    depth_ = depth;
    format_ = format;
    image_type_ = type;

    dk::ImageLayoutMaker maker{texture_cache_.device_.device()};
    maker.setFlags(DkImageFlags_Usage2DEngine)
        .setType(type)
        .setFormat(format)
        .setDimensions(width, height, depth)
        .setMipLevels(1);
    maker.initialize(layout_);
    const uint64_t size = layout_.getSize();
    const uint32_t alignment = layout_.getAlignment();
    if (!size || !alignment) {
      return false;
    }
    const uint64_t aligned = (size + alignment - 1) & ~uint64_t(alignment - 1);
    memory_ = dk::MemBlockMaker{texture_cache_.device_.device(), uint32_t(aligned)}
                  .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
                  .create();
    if (!memory_) {
      REXGPU_ERROR("deko3d: texture image allocation failed ({}x{})", width, height);
      return false;
    }
    image_.initialize(layout_, memory_, 0);
    SetHostMemoryUsage(aligned);
    return true;
  }

  dk::Image& image() { return image_; }
  DkImageFormat format() const { return format_; }
  DkImageType image_type() const { return image_type_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t depth() const { return depth_; }
  bool valid() const { return static_cast<bool>(memory_); }

 private:
  Deko3DTextureCache& texture_cache_;
  dk::UniqueMemBlock memory_;
  dk::Image image_;
  dk::ImageLayout layout_;
  DkImageFormat format_ = DkImageFormat_RGBA8_Unorm;
  DkImageType image_type_ = DkImageType_2D;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t depth_ = 1;
};

Deko3DTextureCache::Deko3DTextureCache(const RegisterFile& register_file,
                                       Deko3DSharedMemory& shared_memory,
                                       ui::deko3d::Deko3DDevice& device)
    : TextureCache(register_file, shared_memory, 1, 1), device_(device) {}

Deko3DTextureCache::~Deko3DTextureCache() = default;

bool Deko3DTextureCache::Initialize() {
  upload_command_memory_ =
      dk::MemBlockMaker{device_.device(), kUploadCommandSize}
          .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
          .create();
  if (!upload_command_memory_) {
    REXGPU_ERROR("deko3d: texture upload command memory allocation failed");
    return false;
  }
  upload_command_buffer_ = dk::CmdBufMaker{device_.device()}.create();
  if (!upload_command_buffer_) {
    REXGPU_ERROR("deko3d: texture upload command buffer creation failed");
    return false;
  }
  REXGPU_INFO("deko3d: texture cache initialized");
  return true;
}

void Deko3DTextureCache::Shutdown() {
  DestroyAllTextures(true);
  upload_command_buffer_ = nullptr;
  upload_command_memory_ = nullptr;
}

std::unique_ptr<TextureCache::Texture> Deko3DTextureCache::CreateTexture(TextureKey key) {
  DkImageFormat format;
  if (!GetDekoImageFormat(key.format, format)) {
    static bool logged_unsupported = false;
    if (!logged_unsupported) {
      logged_unsupported = true;
      REXGPU_WARN("deko3d: unsupported texture format {} (0x{:08X}), skipping",
                  uint32_t(key.format), uint32_t(key.format));
    }
    return nullptr;
  }

  const uint32_t width = key.width_minus_1 + 1;
  const uint32_t height = key.height_minus_1 + 1;
  const uint32_t depth = key.depth_or_array_size_minus_1 + 1;
  DkImageType type = DkImageType_2D;
  switch (key.dimension) {
    case xenos::DataDimension::k1D:
      type = DkImageType_1D;
      break;
    case xenos::DataDimension::k2DOrStacked:
      type = depth > 1 ? DkImageType_2DArray : DkImageType_2D;
      break;
    case xenos::DataDimension::k3D:
      type = DkImageType_3D;
      break;
    case xenos::DataDimension::kCube:
      type = DkImageType_Cubemap;
      break;
    default:
      return nullptr;
  }

  std::unique_ptr<TextureImpl> texture = std::make_unique<TextureImpl>(*this, key);
  if (!texture->Initialize(format, type, width, height, depth)) {
    return nullptr;
  }
  return texture;
}

bool Deko3DTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                                               bool load_base,
                                                               bool load_mips) {
  (void)load_mips;
  if (!load_base) {
    return true;
  }
  if (upload_stopped_) {
    return false;
  }
  TextureImpl& deko_texture = static_cast<TextureImpl&>(texture);
  const TextureKey& key = texture.key();
  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  const texture_util::TextureGuestLayout::Level& base = guest_layout.base;
  if (!base.level_data_extent_bytes) {
    return true;
  }

  // The base-level-only upload is enough to start; mips and arrays are a
  // follow-up once the DKSH pipeline can sample them.
  if (key.depth_or_array_size_minus_1 != 0 ||
      key.dimension == xenos::DataDimension::k3D ||
      key.dimension == xenos::DataDimension::kCube) {
    static bool logged_unsupported = false;
    if (!logged_unsupported) {
      logged_unsupported = true;
      REXGPU_WARN("deko3d: array/3D/cube texture upload not implemented yet; "
                  "using the base level only");
    }
    return true;
  }

  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!format_info) {
    return false;
  }
  const uint32_t bytes_per_block = format_info->bytes_per_block();
  const uint32_t width = deko_texture.width();
  const uint32_t height = deko_texture.height();
  const uint32_t blocks_wide = (width + format_info->block_width - 1) /
                               format_info->block_width;
  const uint32_t blocks_high = (height + format_info->block_height - 1) /
                               format_info->block_height;
  const uint32_t row_bytes = blocks_wide * bytes_per_block;
  const uint32_t total_bytes = row_bytes * blocks_high;
  if (!total_bytes) {
    return true;
  }

  const xenos::TextureFormat base_format = GetBaseFormat(key.format);
  const bool block_compressed = base_format == xenos::TextureFormat::k_DXT1 ||
                                base_format == xenos::TextureFormat::k_DXT2_3 ||
                                base_format == xenos::TextureFormat::k_DXT4_5 ||
                                base_format == xenos::TextureFormat::k_DXN;
  static uint32_t logged_uploads = 0;
  if (logged_uploads < 8) {
    ++logged_uploads;
    REXGPU_WARN(
        "deko3d: texture upload {} of {}x{} format {} -> deko {}, block {}, row {} "
        "bytes, total {} bytes, tiled {}, {} layers",
        logged_uploads, width, height, uint32_t(key.format), uint32_t(deko_texture.format()),
        int(block_compressed), row_bytes, total_bytes, int(key.tiled),
        key.depth_or_array_size_minus_1 + 1);
  }
  // A 2D-engine copy of either of these shapes stalled the queue permanently on
  // hardware - the copy never completed and its fence never signaled, which took
  // the whole renderer down with it (the presenter's RGBA8 uploads into the
  // swapchain, which are 16-byte pitched, have never done this). Refuse them and
  // leave the image alone rather than risk the queue again, until the copy path
  // for these formats is understood.
  if (block_compressed || (row_bytes % 16) != 0) {
    static bool logged_refusal = false;
    if (!logged_refusal) {
      logged_refusal = true;
      REXGPU_WARN(
          "deko3d: not uploading block-compressed or non-16-byte-pitched textures; "
          "they keep whatever the image held until the 2D copy path for them is fixed");
    }
    return true;
  }

  // The size must be a whole number of pages: deko3d allocates the block from
  // the GPU memory pool at page granularity, and a copy that reads past the end
  // of the mapped region does not report an error - it stalls the queue with the
  // fence never signaling (measured: the first texture upload of the first real
  // frame wedged the graphics queue permanently).
  const uint32_t staging_size =
      (total_bytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~uint32_t(DK_MEMBLOCK_ALIGNMENT - 1);
  dk::UniqueMemBlock staging =
      dk::MemBlockMaker{device_.device(), staging_size}
          .setFlags(DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached)
          .create();
  if (!staging) {
    REXGPU_ERROR("deko3d: texture staging allocation failed ({} bytes)", staging_size);
    return false;
  }

  const uint8_t* source = static_cast<Deko3DSharedMemory&>(shared_memory()).cpu_address() +
                          (key.base_page << 12);
  uint8_t* destination = static_cast<uint8_t*>(staging.getCpuAddr());
  const uint32_t endian = uint32_t(key.endianness);
  auto copy_block = [endian](void* output, const void* input, size_t length) {
    texture_conversion::CopySwapBlock(xenos::Endian(endian), output, input, length);
  };

  if (key.tiled) {
    texture_conversion::UntileInfo untile_info{};
    untile_info.offset_x = 0;
    untile_info.offset_y = 0;
    untile_info.width = blocks_wide;
    untile_info.height = blocks_high;
    untile_info.input_pitch = base.row_pitch_bytes / bytes_per_block;
    untile_info.output_pitch = blocks_wide;
    untile_info.input_format_info = format_info;
    untile_info.output_format_info = format_info;
    untile_info.copy_callback = copy_block;
    texture_conversion::Untile(destination, source, &untile_info);
  } else {
    for (uint32_t y = 0; y < blocks_high; ++y) {
      copy_block(destination + size_t(y) * row_bytes,
                 source + size_t(y) * base.row_pitch_bytes, row_bytes);
    }
  }

  if (staging_size > total_bytes) {
    // The copy reads whole pages from this block, so make the tail past the
    // texture's own bytes defined instead of whatever the page held.
    std::memset(destination + total_bytes, 0, staging_size - total_bytes);
  }
  if (!FlushCpuRange(staging, 0, staging_size)) {
    REXGPU_ERROR("deko3d: texture staging cache flush failed");
    return false;
  }

  upload_command_buffer_.clear();
  upload_command_buffer_.addMemory(upload_command_memory_, 0, kUploadCommandSize);
  DkCopyBuf source_buffer{};
  source_buffer.addr = staging.getGpuAddr();
  source_buffer.rowLength = row_bytes;
  source_buffer.imageHeight = total_bytes;
  dk::ImageView destination_view{deko_texture.image()};
  destination_view.setMipLevels(0, 1);
  const DkImageRect destination_rect{0, 0, 0, width, height, 1};
  upload_command_buffer_.copyBufferToImage(source_buffer, destination_view, destination_rect);
  // Drop the GPU's cached image data afterwards: the draws that sample this
  // texture are recorded into a different list, so queue order alone does not
  // guarantee they see the new texels.
  upload_command_buffer_.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  DkCmdList list = upload_command_buffer_.finishList();
  {
    std::lock_guard lock(device_.submit_mutex());
    device_.queue().submitCommands(list);
    device_.queue().signalFence(upload_fence_, true);
  }
  if (!WaitFence(upload_fence_, "texture upload", upload_fence_stats_)) {
    upload_stopped_ = true;
    REXGPU_WARN("deko3d: texture upload stalled; further textures will not be "
                "uploaded and will sample the placeholder instead");
    return false;
  }
  REXGPU_INFO("deko3d: uploaded texture {} ({}x{}, format {})", key.GetLogDimensionName(),
              width, height, uint32_t(key.format));
  return true;
}

bool Deko3DTextureCache::GetBoundTexture(uint32_t fetch_constant, bool want_signed,
                                         BoundTexture& out) {
  out = BoundTexture();
  if (fetch_constant >= xenos::kTextureFetchConstantCount) {
    return false;
  }
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding) {
    return false;
  }
  Texture* texture = want_signed && binding->texture_signed ? binding->texture_signed
                                                            : binding->texture;
  if (!texture) {
    texture = binding->texture_signed ? binding->texture_signed : binding->texture;
  }
  if (!texture) {
    return false;
  }
  // Keep the texture alive while this frame's command list references its image.
  texture->MarkAsUsed();
  TextureImpl& deko_texture = static_cast<TextureImpl&>(*texture);
  if (!deko_texture.valid()) {
    return false;
  }
  out.image = &deko_texture.image();
  out.image_type = deko_texture.image_type();
  out.width = deko_texture.width();
  out.height = deko_texture.height();
  out.depth_or_array_size = texture->key().depth_or_array_size_minus_1 + 1;
  out.dimension = texture->key().dimension;
  return true;
}

}  // namespace rex::graphics::deko3d
