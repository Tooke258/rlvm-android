#include "android/font_engine.h"

#include <android/log.h>

#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

namespace rlvm_android {
namespace {

constexpr char kLogTag[] = "rlvm-font";

// Android 系统字体候选，都是全局可读的。优先 CJK 字体集合。
const char* kFontCandidates[] = {
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJKjp-Regular.otf",
    "/system/fonts/NotoSansCJK-Regular.ttf",
    "/system/fonts/DroidSansFallbackFull.ttf",
    "/system/fonts/DroidSansFallback.ttf",
    nullptr,
};

uint64_t CacheKey(uint32_t codepoint, int size, bool italic, bool bold) {
  return (static_cast<uint64_t>(codepoint) << 32) |
         (static_cast<uint64_t>(size & 0xFFFF) << 16) |
         (italic ? 0x8000u : 0u) | (bold ? 0x4000u : 0u);
}

/** sfnt 名字里的 UTF-16BE 串转成 ASCII（只保留低字节里的 ASCII）。 */
std::string Utf16BeToAscii(const unsigned char* data, unsigned int length) {
  std::string out;
  for (unsigned int i = 0; i + 1 < length; i += 2) {
    const unsigned char low = data[i + 1];
    if (low >= 0x20 && low < 0x7F) out.push_back(static_cast<char>(low));
  }
  return out;
}

std::string FamilyNameOf(FT_Face face) {
  if (face == nullptr) return std::string();

  // FT_IS_SFNT：该字体带 sfnt 表，才能读名字表。
  if (FT_IS_SFNT(face)) {
    const FT_UInt count = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt i = 0; i < count; ++i) {
      FT_SfntName name;
      std::memset(&name, 0, sizeof(name));
      if (FT_Get_Sfnt_Name(face, i, &name) != 0) continue;
      if (name.name_id != TT_NAME_ID_FONT_FAMILY &&
          name.name_id != TT_NAME_ID_PREFERRED_FAMILY) {
        continue;
      }
      if (name.string == nullptr || name.string_len == 0) continue;
      if (name.platform_id == TT_PLATFORM_MICROSOFT) {
        return Utf16BeToAscii(name.string, name.string_len);
      }
      return std::string(reinterpret_cast<const char*>(name.string), name.string_len);
    }
  }
  return face->family_name != nullptr ? face->family_name : std::string();
}

}  // namespace

FontEngine& FontEngine::Instance() {
  static FontEngine instance;
  return instance;
}

bool FontEngine::EnsureLoaded() {
  if (face_ != nullptr) return true;

  if (library_ == nullptr && FT_Init_FreeType(&library_) != 0) {
    last_error_ = "FT_Init_FreeType failed";
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", last_error_.c_str());
    return false;
  }

  for (const char** candidate = kFontCandidates; *candidate != nullptr; ++candidate) {
    if (LoadFromPath(*candidate)) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag, "font loaded: %s",
                          description_.c_str());
      return true;
    }
  }

  last_error_ = "no usable system CJK font found";
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", last_error_.c_str());
  return false;
}

bool FontEngine::LoadFromPath(const std::string& path) {
  // 先问出这个文件里有几张脸（.ttc 是字体集合）。
  long face_count = 1;
  {
    FT_Face probe = nullptr;
    if (FT_New_Face(library_, path.c_str(), 0, &probe) != 0) return false;
    face_count = probe->num_faces;
    FT_Done_Face(probe);
  }

  FT_Face chosen = nullptr;
  long chosen_index = 0;
  std::string chosen_family;

  for (long index = 0; index < face_count; ++index) {
    FT_Face candidate = nullptr;
    if (FT_New_Face(library_, path.c_str(), index, &candidate) != 0) continue;

    const std::string family = FamilyNameOf(candidate);
    const bool japanese = family.find("JP") != std::string::npos ||
                          family.find("Japanese") != std::string::npos;

    // 优先日文那张脸；否则记住第一张可用的。
    if (chosen == nullptr || japanese) {
      if (chosen != nullptr) FT_Done_Face(chosen);
      chosen = candidate;
      chosen_index = index;
      chosen_family = family;
      if (japanese) break;
    } else {
      FT_Done_Face(candidate);
    }
  }

  if (chosen == nullptr) return false;

  face_ = chosen;
  description_ = (chosen_family.empty() ? std::string("<unnamed font>") : chosen_family) +
                 " (index " + std::to_string(chosen_index) + ") from " + path;
  return true;
}

bool FontEngine::SetPixelSize(int pixel_size) {
  if (face_ == nullptr || pixel_size <= 0) return false;
  if (pixel_size == current_pixel_size_) return true;
  if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size)) != 0) return false;
  current_pixel_size_ = pixel_size;
  return true;
}

int FontEngine::Advance(uint32_t codepoint, int pixel_size) {
  if (!EnsureLoaded() || !SetPixelSize(pixel_size)) return 0;
  FT_Set_Transform(face_, nullptr, nullptr);
  if (FT_Load_Char(face_, codepoint, FT_LOAD_DEFAULT) != 0) return pixel_size / 2;
  return static_cast<int>(face_->glyph->advance.x >> 6);
}

const GlyphBitmap* FontEngine::Rasterize(uint32_t codepoint,
                                         int pixel_size,
                                         bool italic,
                                         bool bold) {
  const uint64_t key = CacheKey(codepoint, pixel_size, italic, bold);
  std::map<uint64_t, GlyphBitmap>::iterator cached = cache_.find(key);
  if (cached != cache_.end()) return &cached->second;

  if (!EnsureLoaded() || !SetPixelSize(pixel_size)) return nullptr;

  if (italic) {
    // 合成的斜体：水平剪切。上游用 SDL_ttf 的 TTF_STYLE_ITALIC，效果等价。
    FT_Matrix shear;
    shear.xx = 1 << 16;
    shear.xy = static_cast<FT_Fixed>(0.33 * 65536);
    shear.yx = 0;
    shear.yy = 1 << 16;
    FT_Set_Transform(face_, &shear, nullptr);
  } else {
    FT_Set_Transform(face_, nullptr, nullptr);
  }

  if (FT_Load_Char(face_, codepoint, FT_LOAD_DEFAULT | FT_LOAD_RENDER) != 0)
    return nullptr;

  if (bold) FT_GlyphSlot_Embolden(face_->glyph);

  const FT_Bitmap& bitmap = face_->glyph->bitmap;
  GlyphBitmap glyph;
  glyph.width = static_cast<int>(bitmap.width);
  glyph.height = static_cast<int>(bitmap.rows);
  glyph.advance = static_cast<int>(face_->glyph->advance.x >> 6);

  if (glyph.width > 0 && glyph.height > 0 && bitmap.buffer != nullptr) {
    glyph.coverage.resize(static_cast<size_t>(glyph.width) * glyph.height);
    for (int row = 0; row < glyph.height; ++row) {
      // pitch 可能为负（自底向上的位图），按行取正确的起点。
      const long offset = (bitmap.pitch >= 0)
                              ? static_cast<long>(row) * bitmap.pitch
                              : static_cast<long>(glyph.height - 1 - row) * (-bitmap.pitch);
      std::memcpy(&glyph.coverage[static_cast<size_t>(row) * glyph.width],
                  bitmap.buffer + offset, static_cast<size_t>(glyph.width));
    }
  }

  // 简单上限：字号变化多时避免缓存无限增长。
  if (cache_.size() > 4096) cache_.clear();
  std::map<uint64_t, GlyphBitmap>::iterator inserted =
      cache_.emplace(key, std::move(glyph)).first;
  return &inserted->second;
}

}  // namespace rlvm_android
