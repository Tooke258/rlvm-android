// Android 图形后端（T2.4 阶段）。
//
// 设计取舍：RLVM 的图形架构是「把每一层画到 CPU 表面（DC 0..N），最后合成一帧」。
// 上游的 SDL 后端把合成与混合放在 GPU 上（shaders.cc / texture.cc），那是阶段 4
// 才需要重写的部分。这里先用**纯 CPU 表面**实现 Surface 的全部像素操作，
// 只有最后一步 RenderToScreen* 推屏暂时留空——等 T3.3/T4.2 接上 GLSurfaceView。
//
// 这样做的价值：graphics_object.cc / effects/ 这些上层逻辑可以立刻在真机上跑起来，
// 不必等渲染管线重写完。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_GRAPHICS_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_GRAPHICS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "systems/base/colour_filter.h"
#include "systems/base/graphics_system.h"
#include "systems/base/surface.h"

class AndroidGraphicsSystem;

/**
 * 合成统计。
 *
 * 用来区分「什么都没有画」与「画了但内容本身是黑的」——只看最终帧的非黑像素数
 * 无法分辨这两者，而它们的修法完全不同。
 */
struct GraphicsBlitStats {
  uint64_t calls = 0;           // BlitToSurface 的调用次数
  uint64_t written_pixels = 0;  // 实际写入的目标像素数（有效 alpha）
  uint64_t nonblack_pixels = 0; // 其中写入结果不是黑色的像素数
};

/** 读取并清零累计的合成统计。 */
GraphicsBlitStats TakeGraphicsBlitStats();

/**
 * 开关合成统计。默认关闭——它是逐像素累加，会显著拖慢渲染；
 * 只在需要区分「没画」/「画了但透明」/「画了确实是黑的」时打开。
 */
void SetBlitStatsEnabled(bool enabled);

/**
 * 「谁把屏幕标脏」的统计（诊断帧率用）。
 *
 * 合帧改成脏标记驱动（D-022）之后，如果实际合帧率明显低于主循环轮次，就必须分清
 * "游戏本来就没标脏"和"我们漏标了"。这里按 GraphicsUpdateType（DC0 / HIK / 对象 /
 * 文字 / 鼠标）分别计数：SetDirtyStatsEnabled(true) 打开，TakeDirtyStatsSummary()
 * 读取并清零，返回一行可读文本。
 */
void SetDirtyStatsEnabled(bool enabled);
std::string TakeDirtyStatsSummary();

/** 合成成本：blit 调用数 / 写入像素数 / 累计耗时（µs）；读取即清零。 */
void TakeBlitCostSummary(uint64_t& calls, uint64_t& pixels, uint64_t& time_us);

/** 合成路径计数（fast=整行 memcpy / slow=逐像素 alpha 处理）；读取即清零。 */
void TakeBlitPathSummary(uint64_t& fast_calls, uint64_t& slow_calls);

/** 这一秒里最慢的一次 blit（µs）及其矩形描述；读取即清零。 */
std::string TakeBlitWorstSummary(uint64_t& worst_us);

// 像素格式固定为 0xAABBGGRR：在小端内存中即 R,G,B,A 字节序，
// 与 OpenGL 的 GL_RGBA / GL_UNSIGNED_BYTE 直接对应，后续上传纹理无需转换。
class AndroidSurface : public Surface {
 public:
  // owner 为该表面所属的图形系统；RenderToScreen* 会把像素合成到它的帧缓冲上。
  // 为 nullptr 时这些方法不做任何事（例如临时表面）。
  explicit AndroidSurface(const Size& size,
                          AndroidGraphicsSystem* owner = nullptr);
  ~AndroidSurface() override = default;

  AndroidSurface(const AndroidSurface&) = delete;
  AndroidSurface& operator=(const AndroidSurface&) = delete;

  void Fill(const RGBAColour& colour) override;
  void Fill(const RGBAColour& colour, const Rect& area) override;
  void ToneCurve(const ToneCurveRGBMap effect, const Rect& area) override;
  void Invert(const Rect& area) override;
  void Mono(const Rect& area) override;
  void ApplyColour(const RGBColour& colour, const Rect& area) override;

  // GRP type-2 区域表（「多子图」）：一个文件里可以切出多个子图，
  // 每个子图有自己的矩形与原点偏移。上游的对象渲染全靠 GetPattern：
  // 图形对象调用 objPattNo(n) 选第 n 个子图，取到的矩形就是渲染源矩形。
  // 不实现它时基类返回全 0 矩形，结果是「对象存在、也参与渲染，但源矩形 0x0，
  // 一个像素都画不出来」——真机上表现为整个画面全黑。
  int GetNumPatterns() const override;
  const GrpRect& GetPattern(int patt_no) const override;

  // 设置区域表。空表表示单图资源，自动退化成「整张图一个子图」。
  void SetRegionTable(std::vector<GrpRect> region_table);
  Size GetSize() const override;

  void BlitToSurface(Surface& dest_surface,
                     const Rect& src,
                     const Rect& dst,
                     int alpha = 255,
                     bool use_src_alpha = true) const override;

  // 以下四个方法负责把像素推到屏幕。当前阶段不做任何事：
  // 它们把内容合成到 AndroidGraphicsSystem 的帧缓冲；由 GL 线程负责最终呈现。
  // 这是上游 SDL 后端「RenderToScreen = 送进 GL 管线」的 CPU 等价物。
  void RenderToScreen(const Rect& src,
                      const Rect& dst,
                      int alpha = 255) const override;
  void RenderToScreenAsColorMask(const Rect& src,
                                 const Rect& dst,
                                 const RGBAColour& colour,
                                 int filter) const override;
  void RenderToScreen(const Rect& src,
                      const Rect& dst,
                      const int opacity[4]) const override;
  void RenderToScreenAsObject(const GraphicsObject& rp,
                              const Rect& src,
                              const Rect& dst,
                              int alpha) const override;

  void GetDCPixel(const Point& pos, int& r, int& g, int& b) const override;
  Surface* Clone() const override;

  // 非虚接口：重新分配为指定尺寸并清零。
  void Resize(const Size& size);

  // 用一段 RGBA8888 数据（宽*高*4 字节）替换像素内容。
  // 图像解码器（xclannad 的 GRPCONV）输出的正是这个布局。
  void SetPixelsFromRGBA(const void* rgba);

  // 把一个 8 位灰度覆盖度位图（字形）以给定颜色混合到 (x, y)。
  // 用于文字渲染；刻意不依赖 font_engine，保持图形层独立。
  void BlendCoverage(const uint8_t* coverage,
                     int width,
                     int height,
                     int x,
                     int y,
                     const RGBColour& colour);

  const uint32_t* pixels() const { return pixels_.data(); }

  // ---- 整面不透明标志（合成快路径的依据）------------------------------------
  //
  // 背景层、角色立绘这类资源几乎每个像素的 alpha 都是 255。上游 SDL 后端把这类
  // 合成交给 GL，我们只能在 CPU 上逐像素做；而逐像素走 alpha 混合是 ~33ns/像素，
  // 整屏一次就要十几毫秒，主循环因此掉到 40 帧/秒左右。
  //
  // 因此给每个表面维护一个「是否整面 alpha 全 255」的标志：为真时 blit 可以直接
  // 整段 memcpy（~0.2ns/像素），画面逐像素等价（原路径在 eff_a==255 时也是 d = s）。
  // 标志只在**可能改变 alpha** 的操作后重算或失效，宁可变保守（多走慢路径），
  // 也不能出现画面错误。
  bool pixels_opaque() const { return pixels_opaque_; }
  void InvalidateOpaque() { pixels_opaque_ = false; }
  void SetOpaque(bool opaque) { pixels_opaque_ = opaque; }

 private:
  bool Contains(int x, int y) const;

  // ---- 内容包围盒（不透明区域的保守超集）------------------------------------
  //
  // 为什么需要：合成时源图层常常是"整屏大小、内容只占一角、其余全透明"（文字框、遮罩、
  // 覆盖层都这样）。blit 会逐像素扫过整张源图，实测一次合成里 99.8% 的像素是透明的——
  // 一次合成光扫描就要 ~7ms，主循环因此只能跑到 40~50 帧/秒，动画看起来"被抽帧"。
  //
  // 这里维护一个**保守超集**（只会比真实内容大、绝不会小），blit 时把源矩形裁到它上面；
  // 因为只会裁掉确定全透明的部分，画面完全不变。
  //   · 图像加载/整屏清零 → 重算精确包围盒；
  //   · 局部绘制（Fill 区域、字形混合、blit 到目标）→ 用矩形并集扩展；
  //   · 只改 RGB 的操作（ToneCurve/Invert/Mono/ApplyColour）不动 alpha → 包围盒不变。
  void ComputeExactContentBounds();
  void ExtendContentBounds(int x, int y, int width, int height);
  void ResetContentBoundsToEmpty() { content_x0_ = content_y0_ = 0; content_x1_ = content_y1_ = 0; }
  void ResetContentBoundsToFull() {
    content_x0_ = content_y0_ = 0;
    content_x1_ = size_.width();
    content_y1_ = size_.height();
  }

  int content_x0_ = 0;
  int content_y0_ = 0;
  int content_x1_ = 0;  // 右开区间
  int content_y1_ = 0;  // 下开区间
  // 整面 alpha 是否全为 255（见 pixels_opaque()）。
  bool pixels_opaque_ = false;

  Size size_;
  std::vector<uint32_t> pixels_;
  std::vector<GrpRect> region_table_;
  // 区域表是否由图像加载显式设置（未设置时跟随尺寸自动生成整图子图）。
  bool region_table_explicit_ = false;
  AndroidGraphicsSystem* owner_;
};

// 纯色填充滤镜。上游 SDL 版本会用 GL 把整个对象刷成单色；
// 这里先做空实现（内存测试路径不会触发），等渲染管线就绪再补。
class AndroidColourFilter : public ColourFilter {
 public:
  void Fill(const GraphicsObject& go,
            const Rect& screen_rect,
            const RGBAColour& colour) override;
};

class AndroidGraphicsSystem : public GraphicsSystem {
 public:
  AndroidGraphicsSystem(System& system, Gameexe& gameexe);
  ~AndroidGraphicsSystem() override = default;

  void BeginFrame() override;
  void EndFrame() override;
  std::shared_ptr<Surface> EndFrameToSurface() override;

  void AllocateDC(int dc, Size size) override;
  void SetMinimumSizeForDC(int dc, Size size) override;
  void FreeDC(int dc) override;

  std::shared_ptr<Surface> GetHaikei() override;
  std::shared_ptr<Surface> GetDC(int dc) override;
  std::shared_ptr<Surface> BuildSurface(const Size& size) override;
  ColourFilter* BuildColourFiller() override;
  // 统计"谁把屏幕标脏"（诊断用，见文件头部 SetDirtyStatsEnabled）。
  void MarkScreenAsDirty(GraphicsUpdateType type) override;

  // 当前帧缓冲（合成目标）。GL 线程通过它取像素。
  std::shared_ptr<AndroidSurface> frame_buffer() const { return frame_buffer_; }
  // 已完成的帧数，用于判断画面是否在更新。
  unsigned int frame_count() const { return frame_count_; }

 private:
  // GraphicsSystem 里这个纯虚函数是 private 的，但仍必须实现。
  std::shared_ptr<const Surface> LoadSurfaceFromFile(
      const std::string& short_filename) override;

  std::map<int, std::shared_ptr<AndroidSurface>> display_contexts_;
  std::shared_ptr<AndroidSurface> last_frame_;
  std::shared_ptr<AndroidSurface> frame_buffer_;
  unsigned int frame_count_ = 0;
};

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_ANDROID_GRAPHICS_H_
