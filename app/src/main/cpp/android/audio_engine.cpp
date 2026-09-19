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

  std::vector<char> buffer(static_cast<const char*>(data),
                           static_cast<const char*>(data) + length);
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

    const double frac = position_ - static_cast<double>(index);
    const int16_t* a = in_ + index * kAudioChannels;
    const int16_t* b = a + kAudioChannels;
    for (int ch = 0; ch < kAudioChannels; ++ch) {
      out[produced * kAudioChannels + ch] = static_cast<int16_t>(
          a[ch] + (static_cast<double>(b[ch] - a[ch]) * frac));
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
};

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

  channel.playing.store(false);   // 先停，避免回调读到半更新状态
  channel.ring->Reset();
  {
    std::lock_guard<std::mutex> lock(channel.mutex);
    channel.source = std::move(source);
  }
  channel.volume.store(std::max(0, std::min(255, volume)));
  channel.loop.store(loop);
  channel.playing.store(true);
}

void AudioEngine::Stop(int channel_index) {
  if (channel_index < 0 || channel_index >= static_cast<int>(channels_.size()))
    return;
  Channel& channel = *channels_[channel_index];
  channel.playing.store(false);
  std::lock_guard<std::mutex> lock(channel.mutex);
  channel.source.reset();
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
      if (!channel->playing.load()) continue;
      if (channel->ring->Space() < kChunkFrames) continue;

      std::lock_guard<std::mutex> lock(channel->mutex);
      if (!channel->source) {
        channel->playing.store(false);
        continue;
      }

      const size_t got =
          channel->source->ReadFrames(buffer.data(), kChunkFrames);
      if (got == 0) {
        if (channel->loop.load() && channel->source->Rewind()) {
          did_work = true;
          continue;
        }
        channel->playing.store(false);
        channel->source.reset();
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
    for (size_t i = 0; i < samples; ++i) {
      const int32_t mixed =
          out[i] + (static_cast<int32_t>(scratch[i]) * volume) / 255;
      out[i] = ClampToInt16(mixed);
      const int magnitude = out[i] < 0 ? -static_cast<int>(out[i])
                                       : static_cast<int>(out[i]);
      if (magnitude > *peak) *peak = magnitude;
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
