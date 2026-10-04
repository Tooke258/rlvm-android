#include "android/android_graphics.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <android/log.h>
#include <unistd.h>

#include "android/game_file_system.h"
#include "android/mov_player.h"
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

// D-022 的 blit 优化开关（诊断用）：false = 退回保守路径（不裁剪、不走 memcpy）。
bool g_blit_fast_enabled = true;

// 「谁把屏幕标脏」的统计（诊断用）：见头文件 SetDirtyStatsEnabled 的说明。
bool g_dirty_stats_enabled = false;
uint64_t g_dirty_counts[5] = {0, 0, 0, 0, 0};

// 合成成本探针（D-022）：每秒统计 blit 调用数、写入像素数、累计耗时。
// 两次 NowMicros() 的开销相对一次 blit 可以忽略，因此常开、不设开关。
uint64_t g_blit_time_us = 0;
uint64_t g_blit_pixels = 0;
uint64_t g_blit_calls = 0;
uint64_t g_blit_worst_us = 0;
// 合成路径计数（见 BlitToSurface）：fast = 整行 memcpy，slow = 逐像素处理。
uint64_t g_blit_fast_calls = 0;
uint64_t g_blit_slow_calls = 0;
std::string g_blit_worst_desc;

uint64_t NowMicros() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

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
  // 新分配的表面全是透明像素；包围盒留空，等第一次真正画东西时再扩展。
  ResetContentBoundsToEmpty();
  pixels_opaque_ = false;

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
  ComputeExactContentBounds();
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
  // 字形落在 (x, y, width, height) 内：用这个矩形扩展包围盒（保守但足够紧）。
  ExtendContentBounds(x, y, width, height);
  // 覆盖度混合只会让 alpha 增大（Clamp255(a + coverage)）：原本整面 255 的面
  // 混合后仍是整面 255，所以这里不必失效。
}

Size AndroidSurface::GetSize() const { return size_; }

void AndroidSurface::Fill(const RGBAColour& colour) {
  Fill(colour, Rect(Point(0, 0), size_));
}

void AndroidSurface::Fill(const RGBAColour& colour, const Rect& area) {
  const uint32_t packed =
      PackRGBA(colour.r(), colour.g(), colour.b(), colour.a());
  // 整屏清零（透明）→ 包围盒清空；否则把填充区域并入包围盒。
  const bool whole_surface =
      area.x() <= 0 && area.y() <= 0 && area.width() >= size_.width() &&
      area.height() >= size_.height();
  if (whole_surface) {
    // 整屏填充是热路径（每帧清屏）：一次性 fill 比逐像素 lambda 快得多。
    std::fill(pixels_.begin(), pixels_.end(), packed);
  } else {
    ForEachPixel(area, size_, [&](int x, int y) {
      pixels_[static_cast<size_t>(y) * size_.width() + x] = packed;
    });
  }
  // 整面不透明标志：只有「覆盖全屏且 alpha=255」才能置位，其余一律保守失效。
  //   · 写入的像素带透明（a<255）→ 整面不再保证全不透明；
  //   · 覆盖全屏且 a=255 → 整面全不透明；
  //   · 部分区域且 a=255 → 不建立也不破坏（保持原状态）。
  if (colour.a() != 255) {
    pixels_opaque_ = false;
  } else if (whole_surface) {
    pixels_opaque_ = true;
  }
  if (colour.a() == 0) {
    if (whole_surface) ResetContentBoundsToEmpty();
  } else {
    ExtendContentBounds(area.x(), area.y(), area.width(), area.height());
  }
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

/**
 * 精确重算内容包围盒（alpha != 0 的最小外接矩形），顺带算出「整面是否全不透明」。
 *
 * 两件事共享同一次全图扫描：图片加载本来就要扫一遍求包围盒，这里再多比较一次
 * alpha 是否为 255，几乎不增加成本，却让后续每帧合成能走 memcpy 快路径。
 */
void AndroidSurface::ComputeExactContentBounds() {
  const int w = size_.width();
  const int h = size_.height();
  if (w <= 0 || h <= 0) {
    ResetContentBoundsToEmpty();
    pixels_opaque_ = false;
    return;
  }
  int x0 = w, y0 = h, x1 = 0, y1 = 0;
  bool all_opaque = true;
  for (int y = 0; y < h; ++y) {
    const uint32_t* row = pixels_.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) {
      const int alpha = static_cast<int>(row[x] >> 24);
      if (alpha != 255) all_opaque = false;
      if (alpha == 0) continue;  // 全透明像素不算内容
      if (x < x0) x0 = x;
      if (x >= x1) x1 = x + 1;
      if (y < y0) y0 = y;
      if (y >= y1) y1 = y + 1;
    }
  }
  pixels_opaque_ = all_opaque;
  if (x0 >= x1 || y0 >= y1) {
    ResetContentBoundsToEmpty();
    return;
  }
  content_x0_ = x0;
  content_y0_ = y0;
  content_x1_ = x1;
  content_y1_ = y1;
}

/** 把矩形并入内容包围盒（保守超集）。 */
void AndroidSurface::ExtendContentBounds(int x, int y, int width, int height) {
  if (width <= 0 || height <= 0) return;
  const int x1 = x + width;
  const int y1 = y + height;
  if (content_x0_ >= content_x1_ || content_y0_ >= content_y1_) {
    content_x0_ = x;
    content_y0_ = y;
    content_x1_ = x1;
    content_y1_ = y1;
    return;
  }
  content_x0_ = std::min(content_x0_, x);
  content_y0_ = std::min(content_y0_, y);
  content_x1_ = std::max(content_x1_, x1);
  content_y1_ = std::max(content_y1_, y1);
}

void AndroidSurface::BlitToSurface(Surface& dest_surface,
                                   const Rect& src,
                                   const Rect& dst,
                                   int alpha,
                                   bool use_src_alpha) const {
  AndroidSurface* dest = dynamic_cast<AndroidSurface*>(&dest_surface);
  if (dest == nullptr) return;

  // 源包围盒裁剪（见头文件里的说明）：整屏大小但内容只占一角的图层，其全透明边角
  // 不必逐像素扫过。实测这一条把一次合成从 ~7ms 降到亚毫秒级。
  Rect src_rect = src;
  Rect dst_rect = dst;
  if (g_blit_fast_enabled && content_x0_ < content_x1_ &&
      content_y0_ < content_y1_) {
    const int nx0 = std::max(src.x(), content_x0_);
    const int ny0 = std::max(src.y(), content_y0_);
    const int nx1 = std::min(src.x() + src.width(), content_x1_);
    const int ny1 = std::min(src.y() + src.height(), content_y1_);
    if (nx1 <= nx0 || ny1 <= ny0) return;  // 与内容不相交：整块跳过
    dst_rect = Rect(Point(dst.x() + (nx0 - src.x()), dst.y() + (ny0 - src.y())),
                    Size(nx1 - nx0, ny1 - ny0));
    src_rect = Rect(Point(nx0, ny0), Size(nx1 - nx0, ny1 - ny0));
  }
  // 目标侧的内容包围盒同步扩展（保守超集，只会偏大）。
  dest->ExtendContentBounds(dst_rect.x(), dst_rect.y(), dst_rect.width(),
                            dst_rect.height());

  uint64_t written = 0;
  uint64_t nonblack = 0;

  const Size dst_size = dest->GetSize();
  const int copy_w = std::min(src_rect.width(), dst_rect.width());
  const int copy_h = std::min(src_rect.height(), dst_rect.height());
  const int base_alpha = Clamp255(alpha);

  // 统计只在显式打开时累加（逐像素两个计数器在热路径里很贵）。
  const bool stats = g_blit_stats_enabled;

  const int src_width = size_.width();
  const int dst_width = dst_size.width();

  const uint64_t blit_start_us = NowMicros();
  ++g_blit_calls;
  // 路径计数（诊断）：快路径 = 整行 memcpy；慢路径 = 逐像素 alpha 处理。
  // 专门用来回答「明明是背景/立绘，为什么还这么慢」——即快路径到底有没有命中。
  if (base_alpha == 255 && (!use_src_alpha || pixels_opaque_)) {
    ++g_blit_fast_calls;
  } else {
    ++g_blit_slow_calls;
  }
  // 像素计数走局部累加：热循环里每像素对全局变量做一次读改写会明显拖慢。
  uint64_t local_pixels = 0;

  // 行范围也要收紧到两个表面之内：源/目标矩形可能带超出表面的偏移或尺寸
  //（实测有一次合成里 9 次 blit 只写 6.5K 像素，却要 ~7ms——就是这种空跑几万行）。
  // 这里只影响"跑多少行"，越界行本来就在下面 continue 掉，输出逐像素不变。
  int y_begin = std::max(0, -src_rect.y());
  y_begin = std::max(y_begin, -dst_rect.y());
  int y_end = std::min(copy_h, size_.height() - src_rect.y());
  y_end = std::min(y_end, dst_size.height() - dst_rect.y());

  for (int y = y_begin; y < y_end; ++y) {
    const int sy = src_rect.y() + y;
    const int dy = dst_rect.y() + y;
    if (sy < 0 || sy >= size_.height() || dy < 0 || dy >= dst_size.height()) continue;

    // 把这一行的有效 x 区间一次算好，内层循环里就没有任何边界判断。
    int x_begin = std::max(0, -src_rect.x());
    x_begin = std::max(x_begin, -dst_rect.x());
    int x_end = std::min(copy_w, src_width - src_rect.x());
    x_end = std::min(x_end, dst_width - dst_rect.x());
    if (x_begin >= x_end) continue;

    const uint32_t* s_row =
        pixels_.data() + static_cast<size_t>(sy) * src_width + src_rect.x();
    uint32_t* d_row =
        dest->pixels_.data() + static_cast<size_t>(dy) * dst_width + dst_rect.x();

    // 整行都不透明时直接整段拷贝：这是背景层与全屏推屏最常见的情形。
    // 源面已确认整面 alpha 全 255 时同样成立——原逐像素路径此时走的也是 d = s。
    if (g_blit_fast_enabled && base_alpha == 255 &&
        (!use_src_alpha || pixels_opaque_)) {
      const size_t count = static_cast<size_t>(x_end - x_begin);
      std::memcpy(d_row + x_begin, s_row + x_begin, count * sizeof(uint32_t));
      local_pixels += count;
      if (stats) {
        written += count;
        for (int x = x_begin; x < x_end; ++x) {
          if ((d_row[x] & 0x00FFFFFFu) != 0) ++nonblack;
        }
      }
      continue;
    }

    // base_alpha == 255 时 eff_a 恒等于 src_a，省掉每像素一次乘除——这是
    // 「源面没被判定为整面不透明、又确实需要混合」时最快的补救路径。
    if (base_alpha == 255) {
      for (int x = x_begin; x < x_end; ++x) {
        const uint32_t s = s_row[x];
        const int src_a = AlphaOf(s);
        if (src_a == 0) continue;

        uint32_t& d = d_row[x];
        if (src_a == 255) {
          d = s;
          ++local_pixels;
          if (stats) {
            ++written;
            if ((d & 0x00FFFFFFu) != 0) ++nonblack;
          }
          continue;
        }
        const int inv = 255 - src_a;
        d = PackRGBA((RedOf(s) * src_a + RedOf(d) * inv) / 255,
                     (GreenOf(s) * src_a + GreenOf(d) * inv) / 255,
                     (BlueOf(s) * src_a + BlueOf(d) * inv) / 255,
                     Clamp255(AlphaOf(d) + src_a));
        ++local_pixels;
        if (stats) {
          ++written;
          if ((d & 0x00FFFFFFu) != 0) ++nonblack;
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
        ++local_pixels;
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
      ++local_pixels;
      if (stats) {
        ++written;
        if ((d & 0x00FFFFFFu) != 0) ++nonblack;
      }
    }
  }
  g_blit_pixels += local_pixels;

  // ---- 目标表面的「整面不透明」状态更新 --------------------------------------
  //
  // 这里**不能**无条件失效：DC0（背景）和帧缓冲都是靠 blit 写入的，若无条件清标志，
  // 背景层永远拿不到快路径（实测这会让一次合成卡在 ~13ns/像素）。
  //
  // 正确规则：
  //   · 本次写入的像素不全是不透明 → 目标不再保证整面不透明（失效）；
  //   · 写入全不透明、且本次覆盖整个目标面 → 目标是整面不透明的（建立）；
  //   · 写入全不透明、但只覆盖一部分 → 既不建立也不破坏（保持原状态）。
  const bool writes_fully_opaque =
      (base_alpha == 255) && (!use_src_alpha || pixels_opaque_);
  if (!writes_fully_opaque) {
    dest->InvalidateOpaque();
  } else if (dst.x() <= 0 && dst.y() <= 0 &&
             dst.x() + dst.width() >= dst_size.width() &&
             dst.y() + dst.height() >= dst_size.height()) {
    dest->SetOpaque(true);
  }

  const uint64_t call_us = NowMicros() - blit_start_us;
  g_blit_time_us += call_us;
  // 记录最慢的一次调用（连同它的矩形），用于定位"空跑"的来源。
  if (call_us > g_blit_worst_us) {
    g_blit_worst_us = call_us;
    g_blit_worst_desc =
        std::to_string(src_rect.x()) + "," + std::to_string(src_rect.y()) + " " +
        std::to_string(src_rect.width()) + "x" + std::to_string(src_rect.height()) +
        " -> " + std::to_string(dst_rect.x()) + "," + std::to_string(dst_rect.y()) +
        " src_surface=" + std::to_string(src_width) + "x" +
        std::to_string(size_.height());
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

void SetBlitFastEnabled(bool enabled) { g_blit_fast_enabled = enabled; }

void SetDirtyStatsEnabled(bool enabled) { g_dirty_stats_enabled = enabled; }

std::string TakeDirtyStatsSummary() {
  static const char* kNames[5] = {"dc0", "hik", "obj", "text", "mouse"};
  std::string out;
  for (int i = 0; i < 5; ++i) {
    if (!out.empty()) out += ' ';
    out += kNames[i];
    out += '=';
    out += std::to_string(g_dirty_counts[i]);
    g_dirty_counts[i] = 0;
  }
  return out;
}

std::string TakeBlitWorstSummary(uint64_t& worst_us) {
  worst_us = g_blit_worst_us;
  std::string desc = g_blit_worst_desc;
  g_blit_worst_us = 0;
  g_blit_worst_desc.clear();
  return desc;
}

void TakeBlitCostSummary(uint64_t& calls, uint64_t& pixels, uint64_t& time_us) {
  calls = g_blit_calls;
  pixels = g_blit_pixels;
  time_us = g_blit_time_us;
  g_blit_calls = 0;
  g_blit_pixels = 0;
  g_blit_time_us = 0;
}

/** 合成路径计数（fast=整行 memcpy / slow=逐像素）；读取即清零。 */
void TakeBlitPathSummary(uint64_t& fast_calls, uint64_t& slow_calls) {
  fast_calls = g_blit_fast_calls;
  slow_calls = g_blit_slow_calls;
  g_blit_fast_calls = 0;
  g_blit_slow_calls = 0;
}

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

void AndroidGraphicsSystem::EndFrame() {
  // 影片（v0.2.3 / M3b）：解码在后台线程，这里把「当前该显示的帧」贴到帧缓冲
  // 最上层。没有影片在放时这个调用几乎不花钱（一次队列空判断）。
  if (frame_buffer_) {
    rlvm_android::MovPlayer::Instance().CompositeInto(*frame_buffer_);
  }
  ++frame_count_;
}

void AndroidGraphicsSystem::MarkScreenAsDirty(GraphicsUpdateType type) {
  GraphicsSystem::MarkScreenAsDirty(type);
  if (g_dirty_stats_enabled && type >= 0 && type < 5) {
    ++g_dirty_counts[type];
  }
}

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
