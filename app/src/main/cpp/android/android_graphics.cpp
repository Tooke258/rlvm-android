#include "android/android_graphics.h"

#include <algorithm>

#include "systems/base/colour.h"
#include "utilities/graphics.h"

namespace {

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
}

bool AndroidSurface::Contains(int x, int y) const {
  return x >= 0 && y >= 0 && x < size_.width() && y < size_.height();
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

  const Size dst_size = dest->GetSize();
  const int copy_w = std::min(src.width(), dst.width());
  const int copy_h = std::min(src.height(), dst.height());
  const int base_alpha = Clamp255(alpha);

  for (int y = 0; y < copy_h; ++y) {
    const int sy = src.y() + y;
    const int dy = dst.y() + y;
    if (sy < 0 || sy >= size_.height() || dy < 0 || dy >= dst_size.height()) continue;
    for (int x = 0; x < copy_w; ++x) {
      const int sx = src.x() + x;
      const int dx = dst.x() + x;
      if (sx < 0 || sx >= size_.width() || dx < 0 || dx >= dst_size.width()) continue;

      const uint32_t s = pixels_[static_cast<size_t>(sy) * size_.width() + sx];
      const int src_a = use_src_alpha ? AlphaOf(s) : 255;
      const int eff_a = src_a * base_alpha / 255;
      if (eff_a == 0) continue;

      uint32_t& d = dest->pixels_[static_cast<size_t>(dy) * dst_size.width() + dx];
      if (eff_a == 255) {
        d = s;
        continue;
      }
      const int inv = 255 - eff_a;
      d = PackRGBA((RedOf(s) * eff_a + RedOf(d) * inv) / 255,
                   (GreenOf(s) * eff_a + GreenOf(d) * inv) / 255,
                   (BlueOf(s) * eff_a + BlueOf(d) * inv) / 255,
                   Clamp255(AlphaOf(d) + eff_a));
    }
  }
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
    const std::string& /*short_filename*/) {
  // 图像文件（GRP/GAN/ANM/HIK）的解码与缓存属于 T3.3/T4.2 的工作。
  return std::shared_ptr<const Surface>();
}

void AndroidGraphicsSystem::DrawBringUpPattern() {
  if (!frame_buffer_) return;
  const Size size = frame_buffer_->GetSize();
  if (size.width() <= 0 || size.height() <= 0) return;

  const int bar_height = size.height() / 10;
  const uint32_t colours[8] = {
      PackRGBA(0, 0, 0, 255),       PackRGBA(255, 0, 0, 255),
      PackRGBA(0, 255, 0, 255),     PackRGBA(0, 0, 255, 255),
      PackRGBA(255, 255, 0, 255),   PackRGBA(0, 255, 255, 255),
      PackRGBA(255, 0, 255, 255),   PackRGBA(255, 255, 255, 255),
  };
  for (int i = 0; i < 8; ++i) {
    const int r = static_cast<int>(colours[i] & 0xFF);
    const int g = static_cast<int>((colours[i] >> 8) & 0xFF);
    const int b = static_cast<int>((colours[i] >> 16) & 0xFF);
    frame_buffer_->Fill(RGBAColour(r, g, b, 255),
                        Rect(Point(i * size.width() / 8, 0),
                             Size(size.width() / 8, bar_height)));
  }

  // 一个随帧号横向移动的方块：连续两帧不同，证明画面确实在更新。
  const int box = size.height() / 8;
  const int travel = std::max(1, size.width() - box);
  const int x = static_cast<int>((frame_count_ * 17u) % static_cast<unsigned>(travel));
  const int y = size.height() / 2 - box / 2;
  frame_buffer_->Fill(RGBAColour(255, 255, 255, 255),
                      Rect(Point(x, y), Size(box, box)));
}
