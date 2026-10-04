#include "android/android_graphics.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <android/log.h>
#include <unistd.h>

#include "android/game_file_system.h"
#include "systems/base/colour.h"
#include "systems/base/system.h"
#include "systems/base/system_error.h"
#include "utilities/exception.h"
#include "utilities/graphics.h"
#include "xclannad/file.h"

namespace {

// 合成统计：见头文件里的说明。引擎线程写、探针线程读，用互斥锁即可。
std::mutex g_blit_stats_mutex;
GraphicsBlitStats g_blit_stats;
// 统计默认关闭：它是逐像素累加，放在渲染热路径里纯属浪费。
// 需要时用诊断文件里的 blit_stats=1 打开。
bool g_blit_stats_enabled = false;

constexpr char kGraphicsLogTag[] = "rlvm-graphics";
inline uint32_t PackRGBA(int r, int g, int b, int a) {
  return static_cast<uint32_t>(r & 0xFF) | (static_cast<uint32_t>(g & 0xFF) << 8) |
         (static_cast<uint32_t>(b & 0xFF) << 16) |
         (static_cast<uint32_t>(a & 0xFF) << 24);
}

inline int RedOf(uint32_t p) { return static_cast<int>(p & 0xFF); }
inline int GreenOf(uint32_t p) { return static_cast<int>((p >> 8) & 0xFF); }
inline int BlueOf(uint32_t p) { return static_cast<int>((p >> 16) & 0xFF); }
inline int AlphaOf(uint32_t p) { return static_cast<int>((p >> 24) & 0xFF); }

inline int Clamp255(int v) { return std::max(0, std::min(255, v)); }

// 在 |area| 与 |bounds| 的交集上迭代，避免每个调用点重复裁剪。
template <typename Fn>
void ForEachPixel(const Rect& area, const Size& bounds, Fn fn) {
  const int x0 = std::max(0, area.x());
  const int y0 = std::max(0, area.y());
  const int x1 = std::min(bounds.width(), area.x() + area.width());
  const int y1 = std::min(bounds.height(), area.y() + area.height());
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      fn(x, y);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// AndroidSurface
// ---------------------------------------------------------------------------

AndroidSurface::AndroidSurface(const Size& size, AndroidGraphicsSystem* owner)
    : owner_(owner) {
  Resize(size);
}

void AndroidSurface::Resize(const Size& size) {
  size_ = size;
  const size_t count =
      static_cast<size_t>(std::max(0, size.width())) *
      static_cast<size_t>(std::max(0, size.height()));
  pixels_.assign(count, 0u);

  // 不变式：区域表永远非空——GetPattern 返回引用，空表会越界。
  // 没有显式设置过区域表的表面（DC0/DC1、临时表面）默认「整张图算一个子图」，
  // 尺寸变化时跟着更新。
  if (!region_table_explicit_) {
    GrpRect whole;
    whole.rect = Rect(Point(0, 0), size);
    whole.originX = 0;
    whole.originY = 0;
    region_table_.assign(1, whole);
  }
}

void AndroidSurface::SetRegionTable(std::vector<GrpRect> region_table) {
  region_table_explicit_ = true;
  if (region_table.empty()) {
    GrpRect whole;
    whole.rect = Rect(Point(0, 0), size_);
    whole.originX = 0;
    whole.originY = 0;
    region_table_.assign(1, whole);
    return;
  }
  region_table_ = std::move(region_table);
}

int AndroidSurface::GetNumPatterns() const {
  return static_cast<int>(region_table_.size());
}

// 与上游 SDLSurface 一致：越界的子图编号退回第 0 个
//（RealLive 脚本里常见「文件只有一个子图，但 pattern 号照样给」的写法）。
const Surface::GrpRect& AndroidSurface::GetPattern(int patt_no) const {
  if (patt_no >= 0 && static_cast<size_t>(patt_no) < region_table_.size()) {
    return region_table_[static_cast<size_t>(patt_no)];
  }
  return region_table_[0];
}

bool AndroidSurface::Contains(int x, int y) const {
  return x >= 0 && y >= 0 && x < size_.width() && y < size_.height();
}

void AndroidSurface::SetPixelsFromRGBA(const void* rgba) {
  const size_t bytes = pixels_.size() * sizeof(uint32_t);
  if (rgba == nullptr || bytes == 0) return;
  std::memcpy(pixels_.data(), rgba, bytes);
}

void AndroidSurface::BlendCoverage(const uint8_t* coverage,
                                   int width,
                                   int height,
                                   int x,
                                   int y,
                                   const RGBColour& colour) {
  if (coverage == nullptr || width <= 0 || height <= 0) return;

  for (int row = 0; row < height; ++row) {
    const int py = y + row;
    if (py < 0 || py >= size_.height()) continue;
    for (int column = 0; column < width; ++column) {
      const int px = x + column;
      if (px < 0 || px >= size_.width()) continue;

      const int alpha = coverage[static_cast<size_t>(row) * width + column];
      if (alpha == 0) continue;

      uint32_t& pixel = pixels_[static_cast<size_t>(py) * size_.width() + px];
      const int inverse = 255 - alpha;
      pixel = PackRGBA((colour.r() * alpha + RedOf(pixel) * inverse) / 255,
                       (colour.g() * alpha + GreenOf(pixel) * inverse) / 255,
                       (colour.b() * alpha + BlueOf(pixel) * inverse) / 255,
                       Clamp255(AlphaOf(pixel) + alpha));
    }
  }
}

Size AndroidSurface::GetSize() const { return size_; }

void AndroidSurface::Fill(const RGBAColour& colour) {
  Fill(colour, Rect(Point(0, 0), size_));
}

void AndroidSurface::Fill(const RGBAColour& colour, const Rect& area) {
  const uint32_t packed =
      PackRGBA(colour.r(), colour.g(), colour.b(), colour.a());
  ForEachPixel(area, size_, [&](int x, int y) {
    pixels_[static_cast<size_t>(y) * size_.width() + x] = packed;
  });
}

void AndroidSurface::ToneCurve(const ToneCurveRGBMap effect, const Rect& area) {
  ForEachPixel(area, size_, [&](int x, int y) {
    uint32_t& p = pixels_[static_cast<size_t>(y) * size_.width() + x];
    const int a = AlphaOf(p);
    p = PackRGBA(effect[0][RedOf(p)], effect[1][GreenOf(p)], effect[2][BlueOf(p)],
                 a);
  });
}

void AndroidSurface::Invert(const Rect& area) {
  ForEachPixel(area, size_, [&](int x, int y) {
    uint32_t& p = pixels_[static_cast<size_t>(y) * size_.width() + x];
    p = PackRGBA(255 - RedOf(p), 255 - GreenOf(p), 255 - BlueOf(p), AlphaOf(p));
  });
}

void AndroidSurface::Mono(const Rect& area) {
  ForEachPixel(area, size_, [&](int x, int y) {
    uint32_t& p = pixels_[static_cast<size_t>(y) * size_.width() + x];
    const int grey = (RedOf(p) * 299 + GreenOf(p) * 587 + BlueOf(p) * 114) / 1000;
    p = PackRGBA(grey, grey, grey, AlphaOf(p));
  });
}

void AndroidSurface::ApplyColour(const RGBColour& colour, const Rect& area) {
  ForEachPixel(area, size_, [&](int x, int y) {
    uint32_t& p = pixels_[static_cast<size_t>(y) * size_.width() + x];
    p = PackRGBA(RedOf(p) * colour.r() / 255, GreenOf(p) * colour.g() / 255,
                 BlueOf(p) * colour.b() / 255, AlphaOf(p));
  });
}

void AndroidSurface::BlitToSurface(Surface& dest_surface,
                                   const Rect& src,
                                   const Rect& dst,
                                   int alpha,
                                   bool use_src_alpha) const {
  AndroidSurface* dest = dynamic_cast<AndroidSurface*>(&dest_surface);
  if (dest == nullptr) return;

  uint64_t written = 0;
  uint64_t nonblack = 0;

  const Size dst_size = dest->GetSize();
  const int copy_w = std::min(src.width(), dst.width());
  const int copy_h = std::min(src.height(), dst.height());
  const int base_alpha = Clamp255(alpha);

  // 统计只在显式打开时累加（逐像素两个计数器在热路径里很贵）。
  const bool stats = g_blit_stats_enabled;

  const int src_width = size_.width();
  const int dst_width = dst_size.width();

  for (int y = 0; y < copy_h; ++y) {
    const int sy = src.y() + y;
    const int dy = dst.y() + y;
    if (sy < 0 || sy >= size_.height() || dy < 0 || dy >= dst_size.height()) continue;

    // 把这一行的有效 x 区间一次算好，内层循环里就没有任何边界判断。
    int x_begin = std::max(0, -src.x());
    x_begin = std::max(x_begin, -dst.x());
    int x_end = std::min(copy_w, src_width - src.x());
    x_end = std::min(x_end, dst_width - dst.x());
    if (x_begin >= x_end) continue;

    const uint32_t* s_row =
        pixels_.data() + static_cast<size_t>(sy) * src_width + src.x();
    uint32_t* d_row =
        dest->pixels_.data() + static_cast<size_t>(dy) * dst_width + dst.x();

    // 整行都不透明时直接整段拷贝：这是背景层与全屏推屏最常见的情形。
    if (base_alpha == 255 && !use_src_alpha) {
      const size_t count = static_cast<size_t>(x_end - x_begin);
      std::memcpy(d_row + x_begin, s_row + x_begin, count * sizeof(uint32_t));
      if (stats) {
        written += count;
        for (int x = x_begin; x < x_end; ++x) {
          if ((d_row[x] & 0x00FFFFFFu) != 0) ++nonblack;
        }
      }
      continue;
    }

    for (int x = x_begin; x < x_end; ++x) {
      const uint32_t s = s_row[x];
      const int src_a = use_src_alpha ? AlphaOf(s) : 255;
      const int eff_a = src_a * base_alpha / 255;
      if (eff_a == 0) continue;

      uint32_t& d = d_row[x];
      if (eff_a == 255) {
        d = s;
        if (stats) {
          ++written;
          if ((d & 0x00FFFFFFu) != 0) ++nonblack;
        }
        continue;
      }
      const int inv = 255 - eff_a;
      d = PackRGBA((RedOf(s) * eff_a + RedOf(d) * inv) / 255,
                   (GreenOf(s) * eff_a + GreenOf(d) * inv) / 255,
                   (BlueOf(s) * eff_a + BlueOf(d) * inv) / 255,
                   Clamp255(AlphaOf(d) + eff_a));
      if (stats) {
        ++written;
        if ((d & 0x00FFFFFFu) != 0) ++nonblack;
      }
    }
  }

  if (!stats) return;
  std::lock_guard<std::mutex> lock(g_blit_stats_mutex);
  ++g_blit_stats.calls;
  g_blit_stats.written_pixels += written;
  g_blit_stats.nonblack_pixels += nonblack;
}

GraphicsBlitStats TakeGraphicsBlitStats() {
  std::lock_guard<std::mutex> lock(g_blit_stats_mutex);
  GraphicsBlitStats stats = g_blit_stats;
  g_blit_stats = GraphicsBlitStats();
  return stats;
}

void SetBlitStatsEnabled(bool enabled) { g_blit_stats_enabled = enabled; }

// 「推屏」在 CPU 合成架构下等价于「合成到帧缓冲」：上游 SDL 后端把这些调用送进
// GL 管线，我们则落到 AndroidGraphicsSystem::frame_buffer_ 上，
// 最后由 GL 线程把这一张合成好的图传成纹理显示。
void AndroidSurface::RenderToScreen(const Rect& src, const Rect& dst,
                                    int alpha) const {
  if (owner_ == nullptr) return;
  std::shared_ptr<AndroidSurface> target = owner_->frame_buffer();
  if (!target) return;
  BlitToSurface(*target, src, dst, alpha, true);
}

void AndroidSurface::RenderToScreenAsColorMask(const Rect& src, const Rect& dst,
                                               const RGBAColour& colour,
                                               int /*filter*/) const {
  if (owner_ == nullptr) return;
  std::shared_ptr<AndroidSurface> target = owner_->frame_buffer();
  if (!target) return;
  BlitToSurface(*target, src, dst, colour.a(), true);
  // 近似实现：整块叠加一次色调。真正的逐像素色彩遮罩需要 GL 管线（T4.2）。
  target->ApplyColour(RGBColour(colour.r(), colour.g(), colour.b()), dst);
}

void AndroidSurface::RenderToScreen(const Rect& src, const Rect& dst,
                                    const int opacity[4]) const {
  if (owner_ == nullptr) return;
  std::shared_ptr<AndroidSurface> target = owner_->frame_buffer();
  if (!target) return;
  // 近似实现：四角不透明度取平均。上游用 GL 做双线性插值。
  const int average = (opacity[0] + opacity[1] + opacity[2] + opacity[3]) / 4;
  BlitToSurface(*target, src, dst, average, true);
}

void AndroidSurface::RenderToScreenAsObject(const GraphicsObject& /*rp*/,
                                            const Rect& src, const Rect& dst,
                                            int alpha) const {
  if (owner_ == nullptr) return;
  std::shared_ptr<AndroidSurface> target = owner_->frame_buffer();
  if (!target) return;
  // 近似实现：暂不应用对象自身的色调/亮度调制，只做带 alpha 的合成。
  BlitToSurface(*target, src, dst, alpha, true);
}

void AndroidSurface::GetDCPixel(const Point& pos, int& r, int& g, int& b) const {
  if (!Contains(pos.x(), pos.y())) {
    r = g = b = 0;
    return;
  }
  const uint32_t p = pixels_[static_cast<size_t>(pos.y()) * size_.width() + pos.x()];
  r = RedOf(p);
  g = GreenOf(p);
  b = BlueOf(p);
}

Surface* AndroidSurface::Clone() const {
  AndroidSurface* copy = new AndroidSurface(size_, owner_);
  copy->pixels_ = pixels_;
  copy->region_table_ = region_table_;
  copy->region_table_explicit_ = region_table_explicit_;
  return copy;
}

// ---------------------------------------------------------------------------
// AndroidColourFilter
// ---------------------------------------------------------------------------

void AndroidColourFilter::Fill(const GraphicsObject& /*go*/, const Rect& /*screen_rect*/,
                               const RGBAColour& /*colour*/) {}

// ---------------------------------------------------------------------------
// AndroidGraphicsSystem
// ---------------------------------------------------------------------------

AndroidGraphicsSystem::AndroidGraphicsSystem(System& system, Gameexe& gameexe)
    : GraphicsSystem(system, gameexe) {
  SetScreenSize(GetScreenSize(gameexe));
  // 上游约定：DC0 是背景（haikei），DC1 起是各图形层。
  AllocateDC(0, screen_size());
  AllocateDC(1, screen_size());
  // 合成目标：每帧先把 DC0 与各对象画到这里，再由 GL 线程取走。
  frame_buffer_ = std::make_shared<AndroidSurface>(screen_size(), this);
}

void AndroidGraphicsSystem::BeginFrame() {
  // 每帧从干净缓冲开始（上游 GL 后端同样从清屏开始）。
  if (frame_buffer_) frame_buffer_->Fill(RGBAColour(0, 0, 0, 255));
}

void AndroidGraphicsSystem::EndFrame() { ++frame_count_; }

std::shared_ptr<Surface> AndroidGraphicsSystem::EndFrameToSurface() {
  return frame_buffer_;
}

void AndroidGraphicsSystem::AllocateDC(int dc, Size size) {
  auto& slot = display_contexts_[dc];
  if (!slot) {
    slot = std::make_shared<AndroidSurface>(size, this);
  } else if (slot->GetSize().width() < size.width() ||
             slot->GetSize().height() < size.height()) {
    slot->Resize(size);
  }
}

void AndroidGraphicsSystem::SetMinimumSizeForDC(int dc, Size size) {
  AllocateDC(dc, size);
}

void AndroidGraphicsSystem::FreeDC(int dc) {
  auto it = display_contexts_.find(dc);
  if (it == display_contexts_.end()) return;
  if (dc == 0 || dc == 1) {
    // DC0/DC1 是引擎的基本工作区，清空而不是移除。
    it->second->Fill(RGBAColour(0, 0, 0, 0));
  } else {
    display_contexts_.erase(it);
  }
}

std::shared_ptr<Surface> AndroidGraphicsSystem::GetHaikei() { return GetDC(0); }

std::shared_ptr<Surface> AndroidGraphicsSystem::GetDC(int dc) {
  auto it = display_contexts_.find(dc);
  if (it != display_contexts_.end()) return it->second;
  return std::shared_ptr<Surface>();
}

std::shared_ptr<Surface> AndroidGraphicsSystem::BuildSurface(const Size& size) {
  return std::make_shared<AndroidSurface>(size, this);
}

ColourFilter* AndroidGraphicsSystem::BuildColourFiller() {
  return new AndroidColourFilter();
}

std::shared_ptr<const Surface> AndroidGraphicsSystem::LoadSurfaceFromFile(
    const std::string& short_filename) {
  // 1) 用上游的查找层按 basename + 候选扩展名定位文件。
  //    返回的是不透明文件标识（普通路径后端下是真实路径，SAF 下是相对路径）。
  const boost::filesystem::path file_id =
      system().FindFile(short_filename, IMAGE_FILETYPES);
  if (file_id.empty()) {
    throw rlvm::Exception("Could not find image file \"" + short_filename + "\".");
  }

  std::shared_ptr<rlvm_android::GameFileSystem> file_system =
      rlvm_android::GetGameFileSystem();
  if (!file_system) {
    throw rlvm::Exception("No game file system is installed.");
  }

  // 2) 整份读入内存。图像解码器是缓冲区接口（GRPCONV::AssignConverter），
  //    本来就不按路径读文件，因此这里用 fd 读取即可，SAF 下同样成立。
  const int fd = file_system->OpenFd(file_id.string());
  if (fd < 0) {
    throw rlvm::Exception("Could not open image file: " + file_id.string());
  }
  std::vector<char> data;
  char chunk[16 * 1024];
  for (;;) {
    const ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;
    data.insert(data.end(), chunk, chunk + n);
  }
  close(fd);
  if (data.size() < 10) {
    throw rlvm::Exception("Image file is too small: " + file_id.string());
  }

  // 3) 交给 xclannad 的解码器。AssignConverter 按**内容**分发（PDT / BMP / G00），
  //    与扩展名无关；PNG 与 JPEG 需要 libpng/libjpeg，本移植未启用。
  std::unique_ptr<GRPCONV> converter(
      GRPCONV::AssignConverter(data.data(), static_cast<int>(data.size()),
                               short_filename.c_str()));
  if (converter == NULL) {
    throw SystemError("Failure in GRPCONV: unsupported image format.");
  }

  const int width = converter->Width();
  const int height = converter->Height();
  if (width <= 0 || height <= 0) {
    throw SystemError("Failure in GRPCONV: bad image dimensions.");
  }

  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0u);
  if (!converter->Read(reinterpret_cast<char*>(pixels.data()))) {
    throw SystemError("Failure decoding image: " + short_filename);
  }

  // 通道序转换。xclannad 的解码器输出 BGRA —— 上游据此创建 SDL 表面，掩码为
  // Rmask=0xff0000 / Gmask=0xff00 / Bmask=0xff / Amask=0xff000000，
  // 即内存字节序 B,G,R,A。而本移植的 Surface 用 RGBA（byte0=R），
  // 以便直接以 GL_RGBA / GL_UNSIGNED_BYTE 上传纹理，因此这里交换 R 与 B。
  for (uint32_t& pixel : pixels) {
    const uint32_t red = (pixel >> 16) & 0xFFu;
    const uint32_t blue = pixel & 0xFFu;
    pixel = (pixel & 0xFF00FF00u) | (blue << 16) | red;
  }

  // 4) 掩码判定与上游一致：整张图全不透明时不算掩码。
  bool is_mask = converter->IsMask();
  if (is_mask) {
    const size_t count = static_cast<size_t>(width) * height;
    bool all_opaque = true;
    for (size_t i = 0; i < count; ++i) {
      if ((pixels[i] & 0xff000000u) != 0xff000000u) {
        all_opaque = false;
        break;
      }
    }
    if (all_opaque) is_mask = false;
  }

  std::shared_ptr<AndroidSurface> surface =
      std::make_shared<AndroidSurface>(Size(width, height), this);
  surface->SetPixelsFromRGBA(pixels.data());
  surface->SetIsMask(is_mask);

  // GRP type-2 区域表：解码器已经把每个子图的矩形与原点偏移解析出来了，
  // 这里原样搬进表面。没有区域表的资源退化成「整张图一个子图」。
  // 这一步不能省：对象渲染的源矩形完全来自 GetPattern()。
  std::vector<Surface::GrpRect> region_table;
  region_table.reserve(converter->region_table.size());
  for (const GRPCONV::REGION& region : converter->region_table) {
    Surface::GrpRect pattern;
    // 上游约定：x2/y2 是闭区间，转成半开区间要 +1。
    pattern.rect = Rect(Point(region.x1, region.y1),
                        Point(region.x2 + 1, region.y2 + 1));
    pattern.originX = region.origin_x;
    pattern.originY = region.origin_y;
    region_table.push_back(pattern);
  }
  surface->SetRegionTable(std::move(region_table));

  // 图像加载日志：区分「文件根本没找到」与「找到了但解码结果是空的/全透明」。
  __android_log_print(ANDROID_LOG_INFO, kGraphicsLogTag,
                      "loaded \"%s\" %dx%d mask=%d patterns=%d bytes=%zu",
                      short_filename.c_str(), width, height, is_mask ? 1 : 0,
                      surface->GetNumPatterns(), data.size());

  return surface;
}
