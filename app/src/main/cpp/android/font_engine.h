// 字体引擎（文字渲染）。
//
// 上游通过 SDL_ttf 间接使用 FreeType；本移植直接调用 FreeType。
//
// 字体来源用 Android 系统自带的 CJK 字体（/system/fonts/NotoSansCJK-Regular.ttc，
// 全局可读）。这样既不需要打包字体（msgothic.ttc 是商业字体，不能分发），
// 也不增加 APK 体积。注意它是 CFF 轮廓的 TTC——这正是必须用 FreeType 而不能用
// stb_truetype 之类精简光栅器的原因。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_FONT_ENGINE_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_FONT_ENGINE_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace rlvm_android {

/** 一个已光栅化的字形：8 位灰度覆盖度。 */
struct GlyphBitmap {
  int width = 0;
  int height = 0;
  int advance = 0;
  std::vector<uint8_t> coverage;  // width * height
};

class FontEngine {
 public:
  static FontEngine& Instance();

  /** 幂等：首次调用时加载系统 CJK 字体。失败返回 false 并记录原因。 */
  bool EnsureLoaded();
  bool loaded() const { return face_ != nullptr; }
  /** 形如 "Noto Sans CJK JP (index 0) from /system/fonts/..."，用于日志。 */
  const std::string& description() const { return description_; }
  const std::string& last_error() const { return last_error_; }

  /** 字形推进量（像素）。对应上游 SDLTextSystem::GetCharWidth。 */
  int Advance(uint32_t codepoint, int pixel_size);

  /**
   * 取字形位图（带缓存）。italic 用剪切变换合成斜体，bold 用 FreeType 的合成加粗。
   * 返回 nullptr 表示该码点没有字形。
   */
  const GlyphBitmap* Rasterize(uint32_t codepoint,
                               int pixel_size,
                               bool italic,
                               bool bold);

 private:
  FontEngine() = default;
  ~FontEngine() = default;
  FontEngine(const FontEngine&) = delete;
  FontEngine& operator=(const FontEngine&) = delete;

  bool LoadFromPath(const std::string& path);
  /** 在字体集合里挑日文那一张脸；找不到就返回 0。 */
  int SelectJapaneseFace() const;
  bool SetPixelSize(int pixel_size);

  FT_Library library_ = nullptr;
  FT_Face face_ = nullptr;
  int current_pixel_size_ = 0;
  std::string description_;
  std::string last_error_;
  std::map<uint64_t, GlyphBitmap> cache_;
};

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_FONT_ENGINE_H_
