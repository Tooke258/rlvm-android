// MOV 影片播放（v0.2.3 / M3b）
//
// 通路：自写 MPEG-PS 解复用（Kud Wafter / LBEX 的 MOV/*.mpg 都是 MPEG-1 程序流，
// 平台的 AMediaExtractor 不认 PS，见 docs/DECISIONS.md D-027）→ 按访问单元喂
// 平台解码器（video/mpeg2，CPU 输出 I420）→ YUV→RGBA → 在引擎合帧时贴到帧缓冲
// 的指定矩形上。
//
// 线程模型：解码在**后台线程**（含解码器、解复用状态、帧队列）；引擎线程只调用
// Play/Stop/CompositeInto/playing，靠 mutex + 条件变量交接，解码线程最多领先
// kMaxQueued 帧（这就是影片的节流/最简音画同步）。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PLAYER_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PLAYER_H_

#include <memory>
#include <string>

class Surface;

namespace rlvm_android {

class MovPlayer {
 public:
  static MovPlayer& Instance();

  // 开始播放 file_id（游戏文件标识，如 "MOV/OP00.mpg"），画面贴进
  // (x,y)-(x+w,y+h) 的游戏坐标矩形。max_ms > 0 时播这么久就停（诊断用）。
  // 返回 false 表示打不开文件（脚本里应表现为「什么也没发生」而不是崩）。
  bool Play(const std::string& file_id,
            int x,
            int y,
            int w,
            int h,
            int max_ms = 0);

  // 停播并回收解码线程（mvStop / 关引擎时调用）。
  void Stop();

  // 影片是否还在放（放完/被打断都算 false）。
  bool playing() const;

  // 引擎线程每帧调用（AndroidGraphicsSystem::EndFrame）：把当前该显示的帧贴到
  // dst 上；没有影片、或还没有解出可显示的帧时什么都不做。
  void CompositeInto(Surface& dst);

  // 统计（诊断/日志用）：已解出帧数、为追上时钟丢掉的帧数。
  long long decoded_frames() const;
  long long dropped_frames() const;

 private:
  MovPlayer();
  ~MovPlayer();
  MovPlayer(const MovPlayer&) = delete;
  MovPlayer& operator=(const MovPlayer&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PLAYER_H_
