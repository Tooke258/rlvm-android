// Android 音频后端（T4.1）。
//
// 结构（受 task.md 的硬性约束驱动：音频回调中禁止分配内存、做 I/O、加锁、休眠）：
//
//   解码线程 ──写入──▶ 每通道无锁环形缓冲（SPSC） ──读取──▶ AAudio 回调 ──▶ 设备
//
// 解码用 xclannad 的 WAVFILE 系列（NWA / WAV / OGG），经 MakeConverter 统一转成
// 48kHz 立体声 16-bit —— 正好是 AAudio 的格式，回调里只做加权重采样无关的混音。
//
// 环形缓冲按通道预分配且永不释放，因此音频回调永远持有有效指针、也不需要锁；
// 通道的启停与音量用原子变量表达。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_AUDIO_ENGINE_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_AUDIO_ENGINE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct WAVFILE;

namespace rlvm_android {

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioMaxChannels = 32;

/** 交错立体声 16-bit PCM 的帧数统计。 */
class FrameRing {
 public:
  explicit FrameRing(size_t capacity_frames);

  /** 生产者：写入若干帧，返回实际写入的帧数。 */
  size_t Write(const int16_t* src, size_t frames);
  /** 消费者（音频回调）：读取若干帧，返回实际读到的帧数。 */
  size_t Read(int16_t* dst, size_t frames);

  size_t Available() const;
  size_t Space() const;
  size_t capacity_frames() const { return capacity_; }
  void Reset();

 private:
  std::vector<int16_t> data_;
  size_t capacity_;
  std::atomic<size_t> head_{0};  // 读位置（帧）
  std::atomic<size_t> tail_{0};  // 写位置（帧）
};

/** 可播放的音源：按需解码出交错立体声 16-bit 帧。 */
class AudioSource {
 public:
  virtual ~AudioSource() {}
  virtual size_t ReadFrames(int16_t* out, size_t frames) = 0;
  virtual bool Rewind() = 0;
};

/** 由 xclannad 的 WAVFILE 流驱动的音源。 */
class WavFileSource : public AudioSource {
 public:
  explicit WavFileSource(WAVFILE* file);
  ~WavFileSource() override;

  size_t ReadFrames(int16_t* out, size_t frames) override;
  bool Rewind() override;

 private:
  std::unique_ptr<WAVFILE> file_;
};

class AudioEngine {
 public:
  static AudioEngine& Instance();

  /** 打开 AAudio 输出流并启动解码线程。可重复调用。 */
  bool Start();
  void Shutdown();
  bool IsRunning() const { return running_; }
  /** 最近一次启动失败的原因；成功时为空。 */
  const std::string& LastError() const { return last_error_; }

  /**
   * 从已打开的只读 fd 构造音源。extension 用于选择解码器（wav / nwa / ogg）。
   * 内部 fdopen 后交给 xclannad；失败返回 nullptr。
   */
  std::unique_ptr<AudioSource> OpenSource(int fd, const std::string& extension);

  void Play(int channel, std::unique_ptr<AudioSource> source, bool loop, int volume);
  void Stop(int channel);
  void SetVolume(int channel, int volume);  // 0..255
  bool IsPlaying(int channel) const;

  struct Stats {
    unsigned long long callbacks = 0;
    unsigned long long frames_rendered = 0;
    int peak_amplitude = 0;   // 回调中见过的最大绝对值，用于判断是否真的出声
    int active_channels = 0;
  };
  Stats GetStats() const;
  void ResetPeak();

  // 下面两个仅供音频回调使用。它们被公开只是为了不在头文件里引入 AAudio 类型——
  // 调用者只有 RenderCallback 一个。
  void MixInto(int16_t* out, int16_t* scratch, int32_t frames, int* peak);
  void NoteCallbackFrames(int32_t frames, int peak);

 private:
  AudioEngine() = default;
  ~AudioEngine() = default;
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  struct Channel;
  void DecoderLoop();

  std::vector<std::unique_ptr<Channel>> channels_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_requested_{false};
  std::string last_error_;
  void* stream_ = nullptr;   // AAudioStream*
  int decoder_thread_started_ = 0;

  std::atomic<unsigned long long> callbacks_{0};
  std::atomic<unsigned long long> frames_rendered_{0};
  std::atomic<int> peak_amplitude_{0};
};

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_AUDIO_ENGINE_H_
