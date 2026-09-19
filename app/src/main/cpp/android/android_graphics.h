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

  const uint32_t* pixels() const { return pixels_.data(); }

 private:
  bool Contains(int x, int y) const;

  Size size_;
  std::vector<uint32_t> pixels_;
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
