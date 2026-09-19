#include "android/audio_engine.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "xclannad/wavfile.h"

namespace rlvm_android {
namespace {

constexpr char kLogTag[] = "rlvm-audio";

// 环形缓冲容量：1 秒。按通道预分配（32 通道 × 1 秒 ≈ 6 MB），
// 且永不释放，这样音频回调始终持有有效指针。
constexpr size_t kRingFrames = kAudioSampleRate;
// 解码线程每次填充的帧数。
constexpr size_t kChunkFrames = 4096;
// 回调里用的栈上混音暂存（禁止在回调中分配内存）。
constexpr int32_t kMixBlockFrames = 1024;

long long NowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline int16_t ClampToInt16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

// 软限幅阈值：约 -1.7dBFS。超过它的样本走平滑压缩而不是硬削波。
// 为什么需要：BGM 与语音同时播放时相加很容易顶到满刻度，硬 clamp 会产生
// 刺耳的削波失真（听感就是"音质变差"）。
constexpr int32_t kSoftLimit = 27000;
constexpr int32_t kHardLimit = 32767;

/**
 * Lanczos-2 核：sinc(x) * sinc(x/2)，|x| < 2 之外为 0。
 * 用于重采样插值——比线性/三次插值保留更多高频（见 ResamplingSource 的说明）。
 */
double LanczosWeight(double x) {
  const double ax = x < 0 ? -x : x;
  if (ax < 1e-9) return 1.0;
  if (ax >= 2.0) return 0.0;
  const double px = 3.14159265358979323846 * x;
  return 2.0 * std::sin(px) * std::sin(px / 2.0) / (px * px);
}

/**
 * 软限幅：|x| <= kSoftLimit 时原样返回（绝大多数样本走这条，零额外开销）；
 * 超出部分用 tanh 平滑压缩到 kSoftLimit..kHardLimit 之间。
 */
inline int16_t SoftLimit(int32_t value) {
  if (value > kSoftLimit) {
    const double over = static_cast<double>(value - kSoftLimit) /
                        static_cast<double>(kHardLimit - kSoftLimit);
    return static_cast<int16_t>(kSoftLimit +
                                (kHardLimit - kSoftLimit) * std::tanh(over));
  }
  if (value < -kSoftLimit) {
    const double over = static_cast<double>(-value - kSoftLimit) /
                        static_cast<double>(kHardLimit - kSoftLimit);
    return static_cast<int16_t>(-(kSoftLimit +
                                  (kHardLimit - kSoftLimit) * std::tanh(over)));
  }
  return static_cast<int16_t>(value);
}

aaudio_data_callback_result_t RenderCallback(AAudioStream* /*stream*/,
                                             void* user_data,
                                             void* audio_data,
                                             int32_t num_frames);

}  // namespace

// ---------------------------------------------------------------------------
// FrameRing：单生产者单消费者的无锁环形缓冲（帧为单位，交错立体声）
// ---------------------------------------------------------------------------

FrameRing::FrameRing(size_t capacity_frames)
    : data_(capacity_frames * kAudioChannels, 0), capacity_(capacity_frames) {}

size_t FrameRing::Available() const {
  const size_t tail = tail_.load(std::memory_order_acquire);
  const size_t head = head_.load(std::memory_order_acquire);
  return (tail + capacity_ - head) % capacity_;
}

size_t FrameRing::Space() const {
  // 留一格用于区分「满」与「空」。
  return capacity_ - 1 - Available();
}

size_t FrameRing::Write(const int16_t* src, size_t frames) {
  const size_t space = Space();
  if (frames > space) frames = space;
  size_t tail = tail_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < frames; ++i) {
    const size_t index = (tail + i) % capacity_;
    data_[index * 2] = src[i * 2];
    data_[index * 2 + 1] = src[i * 2 + 1];
  }
  tail_.store((tail + frames) % capacity_, std::memory_order_release);
  return frames;
}

size_t FrameRing::Read(int16_t* dst, size_t frames) {
  const size_t available = Available();
  if (frames > available) frames = available;
  size_t head = head_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < frames; ++i) {
    const size_t index = (head + i) % capacity_;
    dst[i * 2] = data_[index * 2];
    dst[i * 2 + 1] = data_[index * 2 + 1];
  }
  head_.store((head + frames) % capacity_, std::memory_order_release);
  return frames;
}

void FrameRing::Reset() {
  head_.store(0, std::memory_order_release);
  tail_.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// WavFileSource：用 xclannad 的 WAVFILE 流解码
// ---------------------------------------------------------------------------

WavFileSource::WavFileSource(WAVFILE* file) : file_(file) {}

WavFileSource::~WavFileSource() = default;

size_t WavFileSource::ReadFrames(int16_t* out, size_t frames) {
  if (!file_) return 0;
  // blksize=4：一帧立体声 16-bit；blklen=帧数。
  // WAVFILE::Read 返回实际读到的块数（即帧数）。
  const int got =
      file_->Read(reinterpret_cast<char*>(out), 4, static_cast<int>(frames));
  return got > 0 ? static_cast<size_t>(got) : 0;
}

bool WavFileSource::Rewind() {
  if (!file_) return false;
  file_->Seek(0);
  return true;
}

// ---------------------------------------------------------------------------
// 内存 WAV 音源（KOE 语音）
// ---------------------------------------------------------------------------

namespace {

/**
 * 持有内存缓冲的 WAV 音源。
 *
 * 成员声明顺序很重要：buffer_ 必须先声明，inner_ 后声明——这样析构时
 * inner_（含 fmemopen 出来的 FILE*）先被销毁，随后才释放它读取的那块内存。
 */
class MemoryWavSource : public AudioSource {
 public:
  MemoryWavSource(std::vector<char> buffer, std::unique_ptr<AudioSource> inner)
      : buffer_(std::move(buffer)), inner_(std::move(inner)) {}
  ~MemoryWavSource() override = default;

  size_t ReadFrames(int16_t* out, size_t frames) override {
    return inner_ ? inner_->ReadFrames(out, frames) : 0;
  }
  bool Rewind() override { return inner_ ? inner_->Rewind() : false; }

 private:
  std::vector<char> buffer_;
  std::unique_ptr<AudioSource> inner_;
};

}  // namespace

std::unique_ptr<AudioSource> OpenMemoryWavSource(const void* data, size_t length) {
  if (data == nullptr || length < 64) return nullptr;

  // 关键：上游 VoiceSample::Decode() 返回的 size 是**分配大小**（ovk_voice_sample.cc
  // 里写的是 *size = buffer_size），可能比真实数据长；尾部那些未初始化字节如果
  // 被当成音频播出去，听感就是"语音结束时一声爆响"。
  // 这里按 RIFF 头声明的真实长度裁剪，并直接信任 WAV 解析结果。
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  size_t usable = length;
  const auto read_u32 = [](const unsigned char* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  };
  if (length >= 44 && std::memcmp(bytes, "RIFF", 4) == 0) {
    const size_t riff_end = static_cast<size_t>(read_u32(bytes + 4)) + 8;
    if (riff_end >= 44 && riff_end < usable) usable = riff_end;

    size_t off = 12;
    while (off + 8 <= length) {
      const uint32_t chunk_size = read_u32(bytes + off + 4);
      if (std::memcmp(bytes + off, "data", 4) == 0) {
        const size_t data_end = off + 8 + static_cast<size_t>(chunk_size);
        if (data_end < usable) usable = data_end;
        break;
      }
      const size_t next = off + 8 + static_cast<size_t>(chunk_size) +
                          (static_cast<size_t>(chunk_size) & 1u);
      if (next <= off) break;  // 防御：坏 chunk 大小
      off = next;
    }
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "koe wav: allocated=%zu usable=%zu", length, usable);

  std::vector<char> buffer(static_cast<const char*>(data),
                           static_cast<const char*>(data) + usable);
  FILE* file = fmemopen(buffer.data(), buffer.size(), "rb");
  if (file == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "fmemopen failed for koe");
    return nullptr;
  }

  WAVFILE* reader = new WAVFILE_Stream(file, static_cast<int>(buffer.size()));
  const int source_rate = static_cast<int>(reader->wavinfo.SamplingRate);
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "koe source: rate=%d channels=%d bits=%d bytes=%zu",
                      source_rate, reader->wavinfo.Channels,
                      reader->wavinfo.DataBits, buffer.size());

  WAVFILE* converted = WAVFILE::MakeConverter(reader);
  if (converted == nullptr) {
    delete reader;
    return nullptr;
  }

  std::unique_ptr<AudioSource> source(new WavFileSource(converted));
  if (source_rate > 0 && source_rate != WAVFILE::freq) {
    source.reset(new ResamplingSource(std::move(source), source_rate, WAVFILE::freq));
  }
  return std::unique_ptr<AudioSource>(
      new MemoryWavSource(std::move(buffer), std::move(source)));
}

// ---------------------------------------------------------------------------
// ResamplingSource
// ---------------------------------------------------------------------------

ResamplingSource::ResamplingSource(std::unique_ptr<AudioSource> inner,
                                   int source_rate,
                                   int target_rate)
    : inner_(std::move(inner)) {
  if (source_rate > 0 && target_rate > 0 && source_rate != target_rate) {
    step_ = static_cast<double>(source_rate) / static_cast<double>(target_rate);
  }
}

bool ResamplingSource::Refill() {
  // 保留最后一个输入帧作为插值锚点：跨 Refill 的插值必须连续，否则会有周期性爆音。
  size_t kept = 0;
  if (in_len_ > 0) {
    in_[0] = in_[(in_len_ - 1) * kAudioChannels];
    in_[1] = in_[(in_len_ - 1) * kAudioChannels + 1];
    position_ -= static_cast<double>(in_len_ - 1);
    if (position_ < 0.0) position_ = 0.0;
    kept = 1;
  }

  const size_t got = inner_->ReadFrames(in_ + kept * kAudioChannels,
                                       kInputFrames - kept);
  in_len_ = kept + got;
  return got > 0;
}

size_t ResamplingSource::ReadFrames(int16_t* out, size_t frames) {
  if (!inner_ || frames == 0) return 0;

  // 第一次读取时先把输入缓冲填上。
  if (in_len_ == 0 && !Refill()) return 0;

  size_t produced = 0;
  while (produced < frames) {
    const size_t index = static_cast<size_t>(position_);
    if (index + 1 >= in_len_) {
      if (!Refill()) break;  // 输入耗尽：返回已产出的部分
      continue;
    }

    // Lanczos-2 插值：用 position_ 前后各两个采样点加窗 sinc 加权。
    // 线性插值高频衰减明显（听感"闷"、发毛），三次插值好一些但仍有滚降；
    // Lanczos-2 在 44.1k→48k 这种轻度重采样上基本不损失高频。
    const double frac = position_ - static_cast<double>(index);
    const int i1 = static_cast<int>(index);
    const int i0 = (i1 > 0) ? i1 - 1 : 0;
    const int i2 = i1 + 1;
    const int i3 = (i2 + 1 < static_cast<int>(in_len_)) ? i2 + 1 : i2;

    // 四个采样点到实际位置的距离：i0 在 -1 处，i3 在 +2 处。
    const double w0 = LanczosWeight(frac + 1.0);
    const double w1 = LanczosWeight(frac);
    const double w2 = LanczosWeight(1.0 - frac);
    const double w3 = LanczosWeight(2.0 - frac);
    const double norm = w0 + w1 + w2 + w3;

    for (int ch = 0; ch < kAudioChannels; ++ch) {
      const double p0 = in_[i0 * kAudioChannels + ch];
      const double p1 = in_[i1 * kAudioChannels + ch];
      const double p2 = in_[i2 * kAudioChannels + ch];
      const double p3 = in_[i3 * kAudioChannels + ch];

      double value = 0.0;
      if (norm > 1e-9) {
        value = (p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3) / norm;
      } else {
        value = p1;  // 退化情形（几乎不会发生）：保持原样本
      }
      if (value > 32767.0) value = 32767.0;
      if (value < -32768.0) value = -32768.0;
      out[produced * kAudioChannels + ch] = static_cast<int16_t>(value);
    }
    ++produced;
    position_ += step_;
  }
  return produced;
}

bool ResamplingSource::Rewind() {
  in_len_ = 0;
  position_ = 0.0;
  return inner_ ? inner_->Rewind() : false;
}

namespace {

/** 自检用的合成音源：按需生成 index 的 440Hz 正弦（交错立体声）。 */
class SineTestSource : public AudioSource {
 public:
  SineTestSource(int rate, int frames) : rate_(rate), total_(frames) {}

  size_t ReadFrames(int16_t* out, size_t frames) override {
    size_t produced = 0;
    while (produced < frames && pos_ < total_) {
      const double t = static_cast<double>(pos_) / rate_;
      const int16_t sample = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 *
                                                           440.0 * t) *
                                                  20000.0);
      out[produced * 2] = sample;
      out[produced * 2 + 1] = sample;
      ++pos_;
      ++produced;
    }
    return produced;
  }

  bool Rewind() override {
    pos_ = 0;
    return true;
  }

 private:
  int rate_;
  int total_;
  int pos_ = 0;
};

/**
 * 用过零点数估计频率：每秒过零次数 = 2 × 频率。
 *
 * 只看左声道：缓冲区是交错立体声，左右样本相同，若按样本计数会把每次过零
 * 算成一次半（等值样本之间不构成过零），得到恰好一半的频率。
 */
double EstimateFrequency(const std::vector<int16_t>& samples, size_t frames, int rate) {
  int crossings = 0;
  for (size_t i = 1; i < frames; ++i) {
    const int16_t previous = samples[(i - 1) * 2];
    const int16_t current = samples[i * 2];
    if ((previous < 0 && current >= 0) || (previous >= 0 && current < 0)) {
      ++crossings;
    }
  }
  const double seconds = static_cast<double>(frames) / static_cast<double>(rate);
  return (crossings / 2.0) / seconds;
}

}  // namespace

std::string ResamplerSelfTest() {
  std::ostringstream report;

  const int source_rate = 44100;
  const int target_rate = kAudioSampleRate;
  const int source_frames = source_rate;  // 1 秒

  // 1) 直通（不重采样）：把 44.1kHz 数据当 48kHz 播，正是修复前的行为。
  std::unique_ptr<AudioSource> plain(
      new SineTestSource(source_rate, source_frames));
  std::vector<int16_t> plain_out(static_cast<size_t>(target_rate) * 2, 0);
  const size_t plain_got = plain->ReadFrames(plain_out.data(), target_rate);
  plain_out.resize(plain_got * 2);
  report << "audio self-test: passthrough " << source_rate << "Hz -> "
         << target_rate << "Hz gives "
         << EstimateFrequency(plain_out, plain_got, target_rate)
         << " Hz (expected ~" << 440.0 * target_rate / source_rate
         << ", i.e. the 8.8% pitch-up bug)\n";

  // 2) 经重采样：应当仍然是 440Hz。
  std::unique_ptr<AudioSource> resampled(new ResamplingSource(
      std::unique_ptr<AudioSource>(new SineTestSource(source_rate, source_frames)),
      source_rate, target_rate));
  std::vector<int16_t> out(static_cast<size_t>(target_rate) * 2, 0);
  const size_t got = resampled->ReadFrames(out.data(), target_rate);
  out.resize(got * 2);
  report << "audio self-test: resampled " << source_rate << "Hz -> " << target_rate
         << "Hz gives " << EstimateFrequency(out, got, target_rate)
         << " Hz (expected ~440); frames=" << got << "\n";

  // 3) 顺带核对重采样率：1 秒 44.1kHz 输入应产出约 48000 帧。
  report << "audio self-test: expected frames ~" << target_rate
         << ", produced " << got << "\n";
  return report.str();
}

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------

struct AudioEngine::Channel {
  std::unique_ptr<FrameRing> ring;
  // source 只被引擎线程与解码线程访问，因此用互斥量保护是安全的——
  // 音频回调绝不接触它。
  std::mutex mutex;
  std::unique_ptr<AudioSource> source;
  std::atomic<int> volume{255};
  // 音量渐变状态：目标音量、起点音量与起止时刻（毫秒）。
  // 由引擎线程写入、音频回调读取；都在 8 字节以内的原子量上操作。
  std::atomic<int> fade_from{255};
  std::atomic<long long> fade_start_ms{0};
  std::atomic<long long> fade_end_ms{0};
  std::atomic<bool> playing{false};
  std::atomic<bool> loop{false};
  // 音源已读完但环形缓冲还没播完：此时保持 playing，等缓冲排空再停，
  // 否则结尾会被硬截断（见 DecoderLoop 里的说明）。
  std::atomic<bool> draining{false};
  // 停播时的淡出（防咔哒声）：控制线程请求，音频回调逐步衰减到 0 再停通道。
  std::atomic<bool> fade_out_request{false};
  // 仅由音频回调读写的淡出剩余帧数（回调是唯一写入者，无需原子）。
  int fade_out_remaining = 0;
  // 起播淡入剩余帧数：BGM/语音文件的起始样本往往不是零，瞬间满音量起播
  // 会产生一个台阶（听感就是"起播爆点"）。起播时做 2ms 淡入即可消除。
  int fade_in_remaining = 0;
  // 换源交叉淡化：新音源先挂起（受 mutex 保护），等旧内容淡出结束后由解码
  // 线程装上并开始播放。这样"直接切换到新音频"也不会在切换点留下咔哒声。
  std::unique_ptr<AudioSource> pending_source;
  std::atomic<int> pending_volume{255};
  std::atomic<bool> pending_loop{false};
};

// 淡出长度：约 4ms @48kHz。太短压不住咔哒，太长会让人觉得"反应慢"。
constexpr int kFadeOutFrames = 192;
// 起播淡入长度：约 2ms @48kHz。只用来消除起播台阶，不影响听感。
constexpr int kFadeInFrames = 96;
// 主增益余量（约 -2dB）：给"BGM + 语音 + 音效"的和留出空间，
// 避免经常顶到满刻度导致高音破音。
constexpr double kMasterGain = 0.79;

AudioEngine& AudioEngine::Instance() {
  static AudioEngine instance;
  return instance;
}

void AudioEngine::ResetPeak() { peak_amplitude_.store(0); }

AudioEngine::Stats AudioEngine::GetStats() const {
  Stats stats;
  stats.callbacks = callbacks_.load();
  stats.frames_rendered = frames_rendered_.load();
  stats.peak_amplitude = peak_amplitude_.load();
  for (const auto& channel : channels_) {
    if (channel->playing.load()) ++stats.active_channels;
  }
  return stats;
}

bool AudioEngine::Start() {
  if (running_.load()) return true;

  if (channels_.empty()) {
    channels_.reserve(kAudioMaxChannels);
    for (int i = 0; i < kAudioMaxChannels; ++i) {
      auto channel = std::unique_ptr<Channel>(new Channel());
      channel->ring.reset(new FrameRing(kRingFrames));
      channels_.push_back(std::move(channel));
    }
  }

  AAudioStreamBuilder* builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    last_error_ = std::string("AAudio_createStreamBuilder: ") +
                  AAudio_convertResultToText(result);
    return false;
  }

  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setPerformanceMode(builder,
                                         AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(builder, kAudioChannels);
  AAudioStreamBuilder_setSampleRate(builder, kAudioSampleRate);
  AAudioStreamBuilder_setDataCallback(builder, &RenderCallback, this);

  AAudioStream* stream = nullptr;
  result = AAudioStreamBuilder_openStream(builder, &stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    last_error_ = std::string("AAudioStreamBuilder_openStream: ") +
                  AAudio_convertResultToText(result);
    return false;
  }

  stream_ = stream;
  shutdown_requested_.store(false);
  running_.store(true);

  std::thread([this]() { DecoderLoop(); }).detach();
  decoder_thread_started_ = 1;

  result = AAudioStream_requestStart(stream);
  if (result != AAUDIO_OK) {
    last_error_ = std::string("AAudioStream_requestStart: ") +
                  AAudio_convertResultToText(result);
    Shutdown();
    return false;
  }

  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "AAudio stream started: rate=%d channels=%d",
                      kAudioSampleRate, kAudioChannels);
  return true;
}

void AudioEngine::Shutdown() {
  shutdown_requested_.store(true);
  running_.store(false);
  if (stream_ != nullptr) {
    AAudioStream* stream = static_cast<AAudioStream*>(stream_);
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream_ = nullptr;
  }
  // 解码线程会自行观察到 shutdown_requested_ 并退出。
}

std::unique_ptr<AudioSource> AudioEngine::OpenSource(int fd,
                                                     const std::string& extension) {
  if (fd < 0) return nullptr;

  FILE* file = fdopen(fd, "rb");
  if (file == nullptr) {
    close(fd);
    return nullptr;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    fclose(file);
    return nullptr;
  }

  // 与上游 sdl_music.cc 相同的按扩展名选解码器。
  WAVFILE* reader = nullptr;
  if (extension == "nwa") {
    reader = new NWAFILE(file, static_cast<int>(size));
  } else if (extension == "wav") {
    reader = new WAVFILE_Stream(file, static_cast<int>(size));
  } else if (extension == "ogg") {
    reader = new OggFILE(file, static_cast<int>(size));
  } else {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "unsupported audio extension: %s", extension.c_str());
    fclose(file);
    return nullptr;
  }

  // MakeConverter 统一转换到 WAVFILE::freq / format / channels（48kHz 立体声 16-bit）。
  // 无论是否发生转换，返回的指针都拥有底层 reader 的所有权。
  //
  // 注意：上游的 MakeConverter 调 SDL_BuildAudioCVT 时源速率与目标速率都传 freq(48000)，
  // 其自定义的 conv_wave_rate 也只在「源速率 > 48000」时生效。因此低于 48kHz 的
  // 音源不会被重采样——这正是音调偏高的根因。真实速率必须在 MakeConverter
  // 之前取（转换器包装之后拿到的是目标参数），下面据此套一层重采样。
  const int source_rate = static_cast<int>(reader->wavinfo.SamplingRate);
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "source: %s rate=%d channels=%d bits=%d (target %d/%d)",
                      extension.c_str(), source_rate,
                      reader->wavinfo.Channels, reader->wavinfo.DataBits,
                      WAVFILE::freq, WAVFILE::channels);

  WAVFILE* converted = WAVFILE::MakeConverter(reader);
  if (converted == nullptr) {
    delete reader;
    return nullptr;
  }
  std::unique_ptr<AudioSource> source(new WavFileSource(converted));

  // 源速率 != 目标速率时补上重采样（44.1kHz 的 BGM 是主要场景；22.05kHz 的
  // 音效/语音同理，之前也是被当 48kHz 播放）。
  if (source_rate > 0 && source_rate != WAVFILE::freq) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "resampling %d -> %d Hz (ratio %.4f)", source_rate,
                        WAVFILE::freq,
                        static_cast<double>(source_rate) / WAVFILE::freq);
    source.reset(new ResamplingSource(std::move(source), source_rate, WAVFILE::freq));
  }
  return source;
}

void AudioEngine::Play(int channel_index,
                       std::unique_ptr<AudioSource> source,
                       bool loop,
                       int volume) {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return;
  Channel& channel = *channels_[channel_index];
  const int clamped = std::max(0, std::min(255, volume));

  // 正在播（或正在淡出）时不能直接换源：旧波形会从非零值被硬切到新波形，
  // 听感就是切换瞬间的咔哒。改为挂起新源，让旧内容先淡出，再由解码线程接手。
  if (channel.playing.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.pending_source = std::move(source);
    channel.pending_volume.store(clamped);
    channel.pending_loop.store(loop);
    channel.fade_out_request.store(true);
    return;
  }

  channel.playing.store(false);   // 先停，避免回调读到半更新状态
  channel.draining.store(false);
  channel.fade_out_request.store(false);
  channel.ring->Reset();
  {
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.source = std::move(source);
    channel.pending_source.reset();
  }
  channel.volume.store(clamped);
  channel.loop.store(loop);
  // 起播淡入：消除"从 0 直接跳到文件首样本"的台阶（BGM 起播爆点）。
  channel.fade_in_remaining = kFadeInFrames;
  channel.playing.store(true);
}

void AudioEngine::Stop(int channel_index) {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return;
  Channel& channel = *channels_[channel_index];
  // 不立即切断：请求淡出，由音频回调把当前波形衰减到 0 再停通道。
  // 瞬时切断会让波形从非零值直接跳到零，听起来就是"咔"的一声。
  channel.fade_out_request.store(true);
}

void AudioEngine::SetVolume(int channel_index, int volume) {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return;
  Channel& channel = *channels_[channel_index];
  channel.volume.store(std::max(0, std::min(255, volume)));
  channel.fade_end_ms.store(0);  // 立即生效
}

void AudioEngine::FadeVolume(int channel_index,
                             int target_volume,
                             int duration_ms) {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return;
  Channel& channel = *channels_[channel_index];
  const int target = std::max(0, std::min(255, target_volume));

  if (duration_ms <= 0) {
    channel.volume.store(target);
    channel.fade_end_ms.store(0);
    return;
  }

  const long long now = NowMillis();
  channel.fade_from.store(channel.volume.load());
  channel.fade_start_ms.store(now);
  channel.volume.store(target);
  channel.fade_end_ms.store(now + duration_ms);
}

bool AudioEngine::IsPlaying(int channel_index) const {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return false;
  return channels_[channel_index]->playing.load();
}

void AudioEngine::DecoderLoop() {
  std::vector<int16_t> buffer(kChunkFrames * kAudioChannels);

  while (!shutdown_requested_.load()) {
    bool did_work = false;
    for (auto& channel : channels_) {
      // 换源必须在 playing 检查之前处理：旧内容淡出完成后 playing 已被回调置为
      // false，如果放在后面就会被直接跳过，新音源永远装不上。
      {
        std::lock_guard<std::mutex> lock(channel->mutex);
        if (channel->pending_source != nullptr &&
            !channel->fade_out_request.load()) {
          channel->source = std::move(channel->pending_source);
          channel->ring->Reset();
          channel->volume.store(channel->pending_volume.load());
          channel->loop.store(channel->pending_loop.load());
          channel->draining.store(false);
          // 新音源淡入 + 旧内容已淡出 = 交叉淡化，切换点无台阶。
          channel->fade_in_remaining = kFadeInFrames;
          channel->playing.store(true);
          did_work = true;
          continue;
        }
      }

      if (!channel->playing.load()) continue;

      // 收尾：音源已读完（draining）时，等环形缓冲真正排空再停通道。
      // 否则缓冲里剩下的那部分（最多约 1 秒）会被直接丢弃，表现为语音/音效
      // 结尾"不自然地截断"。
      if (channel->draining.load()) {
        if (channel->ring->Available() == 0) {
          channel->draining.store(false);
          channel->playing.store(false);
        }
        continue;
      }

      if (channel->ring->Space() < kChunkFrames) continue;

      std::lock_guard<std::mutex> lock(channel->mutex);
      if (!channel->source) {
        // 源已经不在（被 Stop 或上一轮收尾），走同一条排空路径。
        channel->draining.store(true);
        continue;
      }

      const size_t got =
          channel->source->ReadFrames(buffer.data(), kChunkFrames);
      if (got == 0) {
        if (channel->loop.load() && channel->source->Rewind()) {
          did_work = true;
          continue;
        }
        // 关键：先进入 draining，让回调把缓冲播完，再真正停通道。
        channel->source.reset();
        channel->draining.store(true);
        did_work = true;
        continue;
      }
      channel->ring->Write(buffer.data(), got);
      did_work = true;
    }
    if (!did_work) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  }
}

// 音频回调使用的混音：把各在播通道的样本累加到 out 上并限幅。
// 不分配、不取锁、不做 I/O。
void AudioEngine::MixInto(int16_t* out,
                          int16_t* scratch,
                          int32_t frames,
                          int* peak) {
  const long long now = NowMillis();
  for (auto& channel : channels_) {
    if (!channel->playing.load(std::memory_order_relaxed)) continue;

    const size_t got = channel->ring->Read(scratch, static_cast<size_t>(frames));
    if (got == 0) continue;

    // 音量：若有未结束的渐变，按时间线性插值；否则直接用目标值。
    const int target_volume = channel->volume.load(std::memory_order_relaxed);
    int volume = target_volume;
    const long long fade_end = channel->fade_end_ms.load(std::memory_order_relaxed);
    if (fade_end > 0) {
      const long long fade_start =
          channel->fade_start_ms.load(std::memory_order_relaxed);
      if (now >= fade_end || fade_end <= fade_start) {
        channel->fade_end_ms.store(0, std::memory_order_relaxed);
      } else {
        const int from = channel->fade_from.load(std::memory_order_relaxed);
        const double progress =
            static_cast<double>(now - fade_start) / static_cast<double>(fade_end - fade_start);
        volume = from + static_cast<int>((target_volume - from) * progress);
        volume = std::max(0, std::min(255, volume));
      }
    }

    const size_t samples = got * kAudioChannels;

    // 停播淡出：被请求停止后逐帧把增益降到 0，避免波形被硬切断产生咔哒声。
    int fade_remaining = channel->fade_out_remaining;
    if (fade_remaining <= 0 &&
        channel->fade_out_request.load(std::memory_order_relaxed)) {
      fade_remaining = kFadeOutFrames;
    }
    int fade_in_remaining = channel->fade_in_remaining;
    bool fade_finished = false;

    for (size_t i = 0; i < samples; ++i) {
      int gain = volume;
      if (fade_remaining > 0) {
        gain = volume * fade_remaining / kFadeOutFrames;
        // 交错立体声：每两个样本算一帧，在帧边界上递减。
        if ((i & 1u) == 1u) {
          --fade_remaining;
          if (fade_remaining <= 0) {
            fade_remaining = 0;
            fade_finished = true;
          }
        }
      }
      // 起播淡入：与淡出互斥（换源时旧内容淡出、新内容淡入，天然形成交叉淡化）。
      if (fade_in_remaining > 0 && fade_remaining == 0) {
        gain = gain * (kFadeInFrames - fade_in_remaining) / kFadeInFrames;
        if ((i & 1u) == 1u) {
          --fade_in_remaining;
          if (fade_in_remaining < 0) fade_in_remaining = 0;
        }
      }
      const int32_t mixed =
          out[i] + (static_cast<int32_t>(scratch[i]) * gain) / 255;
      // 主增益余量 + 软限幅：给多通道相加以空间，且避免高音破音。
      out[i] = SoftLimit(static_cast<int32_t>(mixed * kMasterGain));
      const int magnitude = out[i] < 0 ? -static_cast<int>(out[i])
                                       : static_cast<int>(out[i]);
      if (magnitude > *peak) *peak = magnitude;
    }

    channel->fade_out_remaining = fade_remaining;
    channel->fade_in_remaining = fade_in_remaining;
    if (fade_finished) {
      // 淡出完成：真正停掉通道。source 留在原处，由下一次 Play 或 Shutdown 回收
      // ——音频回调里不能加锁去动它。
      channel->fade_out_request.store(false);
      channel->fade_out_remaining = 0;
      channel->playing.store(false);
      channel->draining.store(false);
    }
  }
}

void AudioEngine::NoteCallbackFrames(int32_t frames, int peak) {
  callbacks_.fetch_add(1, std::memory_order_relaxed);
  frames_rendered_.fetch_add(static_cast<unsigned long long>(frames),
                             std::memory_order_relaxed);
  int current = peak_amplitude_.load(std::memory_order_relaxed);
  while (peak > current &&
         !peak_amplitude_.compare_exchange_weak(current, peak)) {
  }
}

namespace {

// 音频回调。约束：不分配内存、不做 I/O、不加锁、不休眠。
aaudio_data_callback_result_t RenderCallback(AAudioStream* /*stream*/,
                                             void* user_data,
                                             void* audio_data,
                                             int32_t num_frames) {
  auto* engine = static_cast<AudioEngine*>(user_data);
  int16_t* out = static_cast<int16_t*>(audio_data);
  std::memset(out, 0, static_cast<size_t>(num_frames) * kAudioChannels *
                          sizeof(int16_t));

  int16_t scratch[kMixBlockFrames * kAudioChannels];
  int peak = 0;
  int32_t done = 0;

  while (done < num_frames) {
    const int32_t block = std::min<int32_t>(num_frames - done, kMixBlockFrames);
    engine->MixInto(out + static_cast<size_t>(done) * kAudioChannels, scratch,
                    block, &peak);
    done += block;
  }

  engine->NoteCallbackFrames(num_frames, peak);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

}  // namespace

}  // namespace rlvm_android
