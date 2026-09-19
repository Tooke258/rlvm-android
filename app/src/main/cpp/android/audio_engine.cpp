#include "android/audio_engine.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
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
// AudioEngine
// ---------------------------------------------------------------------------

struct AudioEngine::Channel {
  std::unique_ptr<FrameRing> ring;
  // source 只被引擎线程与解码线程访问，因此用互斥量保护是安全的——
  // 音频回调绝不接触它。
  std::mutex mutex;
  std::unique_ptr<AudioSource> source;
  std::atomic<int> volume{255};
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
  // 音源不会被重采样。这里把音源的真实参数记录下来，便于核对真实游戏的数据。
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "source: %s rate=%d channels=%d bits=%d (target %d/%d)",
                      extension.c_str(), reader->wavinfo.SamplingRate,
                      reader->wavinfo.Channels, reader->wavinfo.DataBits,
                      WAVFILE::freq, WAVFILE::channels);

  WAVFILE* converted = WAVFILE::MakeConverter(reader);
  if (converted == nullptr) {
    delete reader;
    return nullptr;
  }
  return std::unique_ptr<AudioSource>(new WavFileSource(converted));
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
  channels_[channel_index]->volume.store(std::max(0, std::min(255, volume)));
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
  for (auto& channel : channels_) {
    if (!channel->playing.load(std::memory_order_relaxed)) continue;

    const size_t got = channel->ring->Read(scratch, static_cast<size_t>(frames));
    if (got == 0) continue;

    const int volume = channel->volume.load(std::memory_order_relaxed);
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
