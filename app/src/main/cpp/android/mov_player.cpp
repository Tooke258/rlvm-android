#include "android/mov_player.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "android/android_graphics.h"
#include "android/app_log.h"
#include "android/audio_engine.h"
#include "systems/base/rect.h"
#include "utilities/file.h"

namespace rlvm_android {
namespace {

constexpr char kTag[] = "rlvm-mov";
constexpr int kMaxQueued = 3;        // 解码线程最多领先显示这么多帧
constexpr int kReadChunk = 1 << 16;  // 每次从 fd 读这么多
constexpr size_t kMaxAuBytes = 1 << 20;  // 熔断：单个访问单元的字节上限
constexpr int kFeedFailMax = 200;        // 熔断：连续喂不进输入缓冲的次数

long long SteadyMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

/**
 * I420 → RGBA8888（字节序 R,G,B,A，和引擎 Surface 的 0xAABBGGRR 一致）。
 * 目标尺寸可以跟源不同（`movPlayEx` 给的矩形），用最近邻缩放。
 * BT.601 有限范围，和 mov_probe 里的那份实现一致（探针那份是诊断用）。
 */
void I420ToRgbaScaled(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int sw, int sh, int ystride, int cstride, int dw, int dh,
                      std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(dw) * dh * 4);
  const bool scale_y = (dh != sh);
  const bool scale_x = (dw != sw);
  for (int j = 0; j < dh; ++j) {
    int sy = scale_y ? (j * sh) / dh : j;
    if (sy >= sh) sy = sh - 1;
    const uint8_t* yrow = y + static_cast<size_t>(sy) * ystride;
    const uint8_t* urow = u + static_cast<size_t>(sy / 2) * cstride;
    const uint8_t* vrow = v + static_cast<size_t>(sy / 2) * cstride;
    uint8_t* drow = out.data() + static_cast<size_t>(j) * dw * 4;
    for (int i = 0; i < dw; ++i) {
      int sx = scale_x ? (i * sw) / dw : i;
      if (sx >= sw) sx = sw - 1;
      const int c = static_cast<int>(yrow[sx]) - 16;
      const int d = static_cast<int>(urow[sx / 2]) - 128;
      const int e = static_cast<int>(vrow[sx / 2]) - 128;
      const int cc = c < 0 ? 0 : c;
      int r = (298 * cc + 409 * e + 128) >> 8;
      int g = (298 * cc - 100 * d - 208 * e + 128) >> 8;
      int b = (298 * cc + 516 * d + 128) >> 8;
      r = r < 0 ? 0 : (r > 255 ? 255 : r);
      g = g < 0 ? 0 : (g > 255 ? 255 : g);
      b = b < 0 ? 0 : (b > 255 ? 255 : b);
      drow[i * 4 + 0] = static_cast<uint8_t>(r);
      drow[i * 4 + 1] = static_cast<uint8_t>(g);
      drow[i * 4 + 2] = static_cast<uint8_t>(b);
      drow[i * 4 + 3] = 255;
    }
  }
}

// MPEG-1/2 音频帧头（0xFFEx）：解析出取样率与声道数——配置 Android 音频解码器
// 时这两项是必需的，不给就会 configure 失败（看起来像"没有解码器"）。
bool ParseMp2Header(const uint8_t* p, size_t n, int* rate, int* channels) {
  for (size_t i = 0; i + 4 <= n; ++i) {
    if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
    const int version = (p[i + 1] >> 3) & 0x03;  // 3=MPEG-1, 2=MPEG-2, 0=MPEG-2.5
    const int layer = (p[i + 1] >> 1) & 0x03;    // 2=Layer II, 1=Layer III
    const int sridx = (p[i + 2] >> 2) & 0x03;
    const int mode = (p[i + 3] >> 6) & 0x03;     // 3 = 单声道
    if (layer == 0 || sridx == 3) continue;      // 非法组合
    static const int kMpeg1Rates[3] = {44100, 48000, 32000};
    static const int kMpeg2Rates[3] = {22050, 24000, 16000};
    *rate = (version == 3) ? kMpeg1Rates[sridx] : kMpeg2Rates[sridx];
    *channels = (mode == 3) ? 1 : 2;
    return true;
  }
  return false;
}

size_t FindPictureStart(const std::vector<uint8_t>& b, size_t from) {
  if (b.size() < 4) return std::string::npos;
  const size_t last = b.size() - 4;
  for (size_t i = from; i <= last; ++i) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0x00) {
      return i;
    }
  }
  return std::string::npos;
}

struct Frame {
  std::vector<uint8_t> rgba;  // 已经转成「目标矩形大小」的 RGBA
  int w = 0, h = 0;
  long long pts_ms = 0;
};

/**
 * 影片音频源（v0.2.3 / M4）：把解码出来的 PCM 塞进环形缓冲，交给 AudioEngine
 * 当普通音源用（外面再套一层 ResamplingSource 把 44.1kHz 重采样到 48kHz）。
 *
 * 关键点：**在影片还没放完之前 ReadFrames 绝不返回 0**——AudioEngine 把「0 帧」
 * 当成音源结束，会让通道提前收摊。数据没跟上时用静音顶上（听感上是极短的空白，
 * 但时间轴不会错位）。
 */
class MovieAudioSource : public AudioSource {
 public:
  explicit MovieAudioSource(size_t capacity_frames) : ring_(capacity_frames) {}

  // 影片解码线程调用：写入 PCM（16-bit、双声道）。
  size_t Push(const int16_t* pcm, size_t frames) { return ring_.Write(pcm, frames); }
  size_t Space() const { return ring_.Space(); }
  void MarkEnded() { ended_.store(true); }
  bool ended() const { return ended_.load(); }

  size_t ReadFrames(int16_t* out, size_t frames) override {
    const size_t got = ring_.Read(out, frames);
    if (got > 0) return got;
    if (ended_.load()) return 0;  // 影片放完且缓冲排空：正常收尾
    std::memset(out, 0, frames * kAudioChannels * sizeof(int16_t));
    return frames;  // 还没数据：给静音，别让通道以为放完了
  }
  bool Rewind() override { return false; }

 private:
  FrameRing ring_;
  std::atomic<bool> ended_{false};
};

}  // namespace

struct MovPlayer::Impl {
  // ---- 播放参数（引擎线程写，解码线程只读）----
  std::string file_id;
  int x = 0, y = 0, w = 0, h = 0;
  int max_ms = 0;
  bool loop = false;  // movLoop(2)：放完从头再来，直到 movStop

  std::thread worker;
  std::atomic<bool> stop{false};
  std::atomic<bool> alive{false};   // 解码线程还在跑
  std::atomic<bool> finished{false};  // 放完了（自然结束）
  std::atomic<long long> decoded{0};

  std::mutex m;
  std::condition_variable cv;
  std::deque<Frame> queue;
  Frame current;
  bool has_current = false;
  long long clock_start_ms = 0;  // 第一帧解出来的时刻（影片时钟起点）
  long long dropped = 0;

  // 显示用表面（只有引擎线程碰它）
  std::shared_ptr<AndroidSurface> surface;
  int surface_w = 0, surface_h = 0;

  // ---- 影片音频（M4）----
  // source 交给 AudioEngine 之后由它持有；解码线程用裸指针往环形缓冲推 PCM。
  std::unique_ptr<MovieAudioSource> audio_source;
  MovieAudioSource* audio_source_raw = nullptr;
  std::atomic<bool> audio_started{false};
  std::atomic<bool> audio_failed{false};
  int audio_src_channels = 2;
  // 音频解码线程用（只在解码线程里碰）
  AMediaCodec* audio_codec = nullptr;
  AMediaFormat* audio_fmt = nullptr;

  // 影片解码线程拿到音频输出格式后调：起播音频通道（带重采样）。
  void StartAudioIfNeeded(int rate, int channels, int encoding);
  // 把一段 PCM（解码器输出）推给音频源；ring 满时等引擎线程消费。
  void PushPcm(const uint8_t* pcm, size_t bytes);
  void PushLoop(MovieAudioSource* dst, const int16_t* pcm, size_t frames);

  int QueueDepth() {
    std::lock_guard<std::mutex> lock(m);
    return static_cast<int>(queue.size());
  }

  // 解码线程主体：fd 是已经打开的影片文件。
  void DecodeLoop(int fd);
};

MovPlayer::MovPlayer() : impl_(new Impl) {}

MovPlayer::~MovPlayer() {
  Stop();
}

MovPlayer& MovPlayer::Instance() {
  static MovPlayer player;
  return player;
}

bool MovPlayer::Play(const std::string& file_id, int x, int y, int w, int h,
                     int max_ms, bool loop) {
  // 同一部影片正在播时不要推倒重来：KW/LBEX 的 OP 场景会连着调
  // `movPlayEx("OP00",...)` 与 `movPlayExC("OP00",...)`，重启一次会看到明显顿挫。
  if (playing() && impl_->file_id == file_id) {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->x = x;
    impl_->y = y;
    impl_->w = w;
    impl_->h = h;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "mov: %s 已在播，忽略重复起播（只更新矩形）",
                        file_id.c_str());
    return true;
  }
  Stop();  // 一次只放一个影片

  const int fd = OpenGameFileFd(file_id);
  if (fd < 0) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "mov: 打不开 %s", file_id.c_str());
    AppendAppLogLine("mov: 打不开 " + file_id + "（OpenGameFileFd 返回 -1）");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->file_id = file_id;
    impl_->x = x;
    impl_->y = y;
    impl_->w = w;
    impl_->h = h;
    impl_->max_ms = max_ms;
    impl_->loop = loop;
    impl_->queue.clear();
    impl_->has_current = false;
    impl_->clock_start_ms = 0;
    impl_->dropped = 0;
  }
  // 音频：先把源准备好（真正起播要等解码器报出取样率/声道数）。
  impl_->audio_started = false;
  impl_->audio_failed = false;
  impl_->audio_src_channels = 2;
  impl_->audio_source.reset(new MovieAudioSource(kAudioSampleRate * 2));  // 2 秒
  impl_->audio_source_raw = impl_->audio_source.get();
  impl_->decoded = 0;
  impl_->stop = false;
  impl_->finished = false;
  impl_->alive = true;
  impl_->worker = std::thread([this, fd]() { impl_->DecodeLoop(fd); });
  __android_log_print(ANDROID_LOG_INFO, kTag, "mov: play %s rect=%d,%d %dx%d",
                      file_id.c_str(), x, y, w, h);
  AppendAppLogLine("mov: 起播 " + file_id + " 矩形 " + std::to_string(x) + "," +
                   std::to_string(y) + " " + std::to_string(w) + "x" +
                   std::to_string(h));
  return true;
}

void MovPlayer::Stop() {
  if (impl_->worker.joinable()) {
    impl_->stop = true;
    impl_->cv.notify_all();
    impl_->worker.join();
  }
  // 音频通道要在解码线程停掉之后再关（源对象归 AudioEngine 所有）。
  AudioEngine::Instance().Stop(kMovieAudioChannel);
  impl_->audio_source_raw = nullptr;
  impl_->audio_source.reset();
  impl_->audio_started = false;
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->alive = false;
  impl_->queue.clear();
  impl_->has_current = false;
}

bool MovPlayer::playing() const {
  return impl_->alive.load() && !impl_->finished.load();
}

long long MovPlayer::decoded_frames() const { return impl_->decoded.load(); }

long long MovPlayer::dropped_frames() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->dropped;
}

void MovPlayer::CompositeInto(Surface& dst) {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->queue.empty() && !impl_->has_current) return;
    if (impl_->clock_start_ms == 0 && !impl_->queue.empty()) {
      impl_->clock_start_ms = SteadyMs();  // 影片时钟从「第一帧可用」起算
    }
    if (impl_->clock_start_ms == 0) return;
    const long long now = SteadyMs() - impl_->clock_start_ms;
    // 放完后的收尾：影片已经结束、队列也空了、最后一帧也展示过了 → 收起画面，
    // 让屏幕回到游戏。不收的话最后一帧会一直贴在帧缓冲上（看起来像"卡住"）。
    if (impl_->finished.load() && impl_->queue.empty() &&
        impl_->has_current && now >= impl_->current.pts_ms + 120) {
      impl_->has_current = false;
      return;
    }
    // 取「最后一帧 pts <= 当前时刻」；更早的丢掉（渲染跟不上时自动跳帧）。
    while (!impl_->queue.empty()) {
      const bool last_one =
          impl_->finished.load() && impl_->queue.size() == 1;
      if (impl_->queue.front().pts_ms <= now || last_one) {
        if (impl_->has_current) ++impl_->dropped;
        impl_->current = std::move(impl_->queue.front());
        impl_->queue.pop_front();
        impl_->has_current = true;
      } else {
        break;
      }
    }
    if (!impl_->has_current) return;
  }
  impl_->cv.notify_all();  // 让解码线程继续填队列

  const Frame& f = impl_->current;
  if (f.w <= 0 || f.h <= 0 ||
      f.rgba.size() != static_cast<size_t>(f.w) * f.h * 4) {
    return;
  }
  if (!impl_->surface || impl_->surface_w != f.w || impl_->surface_h != f.h) {
    impl_->surface = std::make_shared<AndroidSurface>(Size(f.w, f.h), nullptr);
    impl_->surface_w = f.w;
    impl_->surface_h = f.h;
  }
  impl_->surface->SetPixelsFromRGBA(f.rgba.data());
  impl_->surface->SetOpaque(true);
  impl_->surface->BlitToSurface(dst, Rect::REC(0, 0, f.w, f.h),
                                Rect::REC(impl_->x, impl_->y, impl_->w, impl_->h),
                                255, false);
}

void MovPlayer::Impl::StartAudioIfNeeded(int rate, int channels, int encoding) {
  if (audio_started.load() || audio_failed.load()) return;
  if (rate <= 0 || channels <= 0) return;
  // 只认 16-bit PCM；单声道复制成双声道（引擎的混音是固定的双声道）。
  if (encoding != 0 && encoding != 2) {
    AppendAppLogLine("mov: 音频 PCM 编码 " + std::to_string(encoding) +
                     " 不支持，只有画面");
    audio_failed = true;
    return;
  }
  if (channels != 1 && channels != 2) {
    AppendAppLogLine("mov: 音频 " + std::to_string(channels) + " 声道不支持，只有画面");
    audio_failed = true;
    return;
  }
  std::unique_ptr<MovieAudioSource> src = std::move(audio_source);
  if (src == nullptr) return;
  audio_src_channels = channels;
  audio_started = true;
  // AudioEngine 是**懒启动**的（游戏放第一段声音时才开 AAudio 流），影片可能比
  // 它更早出声，所以这里要自己保证引擎起来了，否则通道根本不存在（Play 直接返回）。
  AudioEngine& engine = AudioEngine::Instance();
  if (!engine.Start()) {
    AppendAppLogLine("mov: 音频引擎启动失败（" + engine.LastError() +
                     "），本片只有画面");
    audio_failed = true;
    audio_started = false;
    return;
  }
  // 用现成的 ResamplingSource 把影片的取样率转到引擎的 48kHz（BGM 也是这条路）。
  auto source = std::unique_ptr<AudioSource>(
      new ResamplingSource(std::move(src), rate, kAudioSampleRate));
  engine.Play(kMovieAudioChannel, std::move(source), false, 255);
  AppendAppLogLine("mov: 音频起播 " + std::to_string(rate) + "Hz " +
                   std::to_string(channels) + "ch → 48kHz");
}

void MovPlayer::Impl::PushPcm(const uint8_t* pcm, size_t bytes) {
  MovieAudioSource* dst = audio_source_raw;
  if (dst == nullptr || pcm == nullptr || bytes < 2) return;
  const int16_t* in = reinterpret_cast<const int16_t*>(pcm);
  const size_t in_channels = static_cast<size_t>(audio_src_channels);
  const size_t frames = bytes / 2 / in_channels;
  std::vector<int16_t> stereo;
  if (in_channels == 2) {
    PushLoop(dst, in, frames);
  } else {
    stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
      stereo[i * 2] = in[i];
      stereo[i * 2 + 1] = in[i];
    }
    PushLoop(dst, stereo.data(), frames);
  }
}

// 环形缓冲满时等音频引擎消费（解码线程自己不能一直空转）。
void MovPlayer::Impl::PushLoop(MovieAudioSource* dst, const int16_t* pcm,
                               size_t frames) {
  size_t off = 0;
  int guard = 0;
  while (off < frames && !stop.load()) {
    const size_t put = dst->Push(pcm + off * kAudioChannels, frames - off);
    off += put;
    if (put == 0 && ++guard > 2000) break;  // 熔断：约 20 秒还没消费完
    if (put == 0) {
      std::unique_lock<std::mutex> lock(m);
      cv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
        return stop.load() || dst->Space() > 0;
      });
    } else {
      guard = 0;
    }
  }
}

void MovPlayer::Impl::DecodeLoop(int fd) {
  AMediaCodec* codec = nullptr;
  AMediaFormat* fmt = nullptr;
  int sw = 0, sh = 0, stride = 0, slice_h = 0, color_fmt = 0;
  bool codec_started = false;
  bool eos_sent = false, eos_seen = false;
  long long au_index = 0;
  std::vector<uint8_t> buf;  // PS 读缓冲
  size_t pos = 0;
  std::vector<uint8_t> au;  // 当前访问单元（ES 字节）
  std::vector<uint8_t> au_audio_hint;  // 头几 KB 音频 ES：用来解 MP2 帧头
  size_t au_scan = 0;
  bool saw_first_picture = false;
  const long long t0 = SteadyMs();

  auto ensure = [&](size_t need) -> bool {
    while (buf.size() - pos < need) {
      const size_t old = buf.size();
      buf.resize(old + kReadChunk);
      const ssize_t n = ::read(fd, buf.data() + old, kReadChunk);
      if (n <= 0) {
        buf.resize(old);
        return false;
      }
      buf.resize(old + static_cast<size_t>(n));
    }
    return true;
  };

  auto drain = [&]() {
    for (int guard = 0; guard < 64 && !stop; ++guard) {
      AMediaCodecBufferInfo info;
      const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 0);
      if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
        if (of != nullptr) {
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &sw);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &sh);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_fmt);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_h);
          AMediaFormat_delete(of);
        }
        continue;
      }
      if (ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
      if (ob < 0) break;  // TRY_AGAIN：暂时没有更多输出

      size_t cap = 0;
      uint8_t* obuf = AMediaCodec_getOutputBuffer(codec, (size_t)ob, &cap);
      if (obuf != nullptr && color_fmt == 19 && sw > 0 && sh > 0) {
        const int ew = stride > 0 ? stride : sw;
        const int eh = slice_h > 0 ? slice_h : sh;
        if (cap >= static_cast<size_t>(ew) * eh * 3 / 2) {
          Frame f;
          f.w = w;
          f.h = h;
          f.pts_ms = info.presentationTimeUs / 1000;
          I420ToRgbaScaled(obuf, obuf + static_cast<size_t>(ew) * eh,
                           obuf + static_cast<size_t>(ew) * eh +
                               static_cast<size_t>(ew / 2) * (eh / 2),
                           sw, sh, ew, ew / 2, w, h, f.rgba);
          {
            std::lock_guard<std::mutex> lock(m);
            queue.push_back(std::move(f));
          }
          ++decoded;
        }
      }
      AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
        eos_seen = true;
      }
      // 队列满：等引擎线程取帧（节流；这也是最简的音画同步）
      while (!stop && QueueDepth() >= kMaxQueued) {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(10),
                    [&]() { return stop.load() || queue.size() < kMaxQueued; });
      }
    }
  };

  auto ensure_codec = [&](const uint8_t* es_prefix, size_t prefix_len) -> bool {
    if (codec_started) return true;
    // csd-0 = 序列头（00 00 01 B3 到下一个起始码）
    size_t b3 = std::string::npos;
    for (size_t i = 0; i + 4 <= prefix_len; ++i) {
      if (es_prefix[i] == 0 && es_prefix[i + 1] == 0 && es_prefix[i + 2] == 1 &&
          es_prefix[i + 3] == 0xB3) {
        b3 = i;
        break;
      }
    }
    if (b3 == std::string::npos) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "mov: ES 里没有序列头");
      return false;
    }
    size_t end = prefix_len;
    for (size_t i = b3 + 4; i + 4 <= prefix_len; ++i) {
      if (es_prefix[i] == 0 && es_prefix[i + 1] == 0 && es_prefix[i + 2] == 1) {
        end = i;
        break;
      }
    }
    const uint8_t* p = es_prefix + b3;
    if (end - b3 >= 8) {
      sw = (p[4] << 4) | (p[5] >> 4);
      sh = ((p[5] & 0x0F) << 8) | p[6];
    }
    fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/mpeg2");
    AMediaFormat_setBuffer(fmt, "csd-0", p, end - b3);
    if (sw > 0 && sh > 0) {
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, sw);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, sh);
    }
    codec = AMediaCodec_createDecoderByType("video/mpeg2");
    media_status_t st = codec == nullptr ? AMEDIA_ERROR_UNKNOWN
                                         : AMediaCodec_configure(codec, fmt,
                                                                 nullptr, nullptr, 0);
    if (st == AMEDIA_OK) st = AMediaCodec_start(codec);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "mov: 解码器 csd=%zu 字节 %dx%d configure/start -> %d",
                        end - b3, sw, sh, (int)st);
    if (st != AMEDIA_OK) return false;
    codec_started = true;
    return true;
  };

  // 等一个输入缓冲并按需分批喂完整段 AU（一个输入缓冲正常情况下就是一个 AU）。
  auto feed = [&](const uint8_t* data, size_t n) -> bool {
    size_t off = 0;
    int fail = 0;
    while (off < n && !stop) {
      if (++fail > kFeedFailMax) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "mov: 熔断（喂不进输入缓冲）");
        return false;
      }
      const ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 5000);
      if (ib < 0) {
        drain();
        continue;
      }
      size_t cap = 0;
      uint8_t* in = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
      if (in == nullptr || cap == 0) continue;
      const size_t put = std::min(cap, n - off);
      std::memcpy(in, data + off, put);
      AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, put,
                                   static_cast<uint64_t>(au_index) * 33366ull, 0);
      off += put;
      drain();
    }
    ++au_index;
    return off >= n;
  };

  // ---- 音频（M4）-------------------------------------------------------
  // 与视频共用同一个解复用游标：音画天然对应到同一个文件位置。
  auto ensure_audio_codec = [&]() -> bool {
    if (audio_codec != nullptr) return true;
    if (audio_failed.load()) return false;
    // 先解析 MP2 帧头：Android 的音频解码器 configure 必须要 sample-rate /
    // channel-count，不给就会失败（现象是「一个解码器都建不起来」）。
    int rate = 0, channels = 0;
    if (!ParseMp2Header(au_audio_hint.data(), au_audio_hint.size(), &rate,
                        &channels)) {
      return false;  // 还没拿到完整帧头，等下一包
    }
    static const char* kMimes[] = {"audio/mpeg", "audio/mpeg-L2", "audio/mp3"};
    for (const char* mime : kMimes) {
      AMediaCodec* c = AMediaCodec_createDecoderByType(mime);
      if (c == nullptr) continue;
      AMediaFormat* f = AMediaFormat_new();
      AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
      AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate);
      AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
      media_status_t st = AMediaCodec_configure(c, f, nullptr, nullptr, 0);
      if (st == AMEDIA_OK) st = AMediaCodec_start(c);
      if (st == AMEDIA_OK) {
        audio_codec = c;
        audio_fmt = f;
        AppendAppLogLine(std::string("mov: 音频解码器 = ") + mime + "  " +
                         std::to_string(rate) + "Hz " +
                         std::to_string(channels) + "ch");
        return true;
      }
      AppendAppLogLine(std::string("mov: 音频解码器 ") + mime +
                       " configure 失败 -> " + std::to_string((int)st));
      AMediaCodec_delete(c);
      AMediaFormat_delete(f);
    }
    AppendAppLogLine("mov: 没有能解这款 MP2 的音频解码器，本片只出画面");
    audio_failed = true;
    return false;
  };

  auto drain_audio = [&]() {
    if (audio_codec == nullptr) return;
    for (int g = 0; g < 64 && !stop; ++g) {
      AMediaCodecBufferInfo info;
      const ssize_t ob = AMediaCodec_dequeueOutputBuffer(audio_codec, &info, 0);
      if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* of = AMediaCodec_getOutputFormat(audio_codec);
        int rate = 0, ch = 0, enc = 2;
        if (of != nullptr) {
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
          if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_PCM_ENCODING, &enc)) {
            enc = 2;
          }
          AMediaFormat_delete(of);
        }
        StartAudioIfNeeded(rate, ch, enc);
        continue;
      }
      if (ob < 0) break;
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getOutputBuffer(audio_codec, (size_t)ob, &cap);
      if (buf != nullptr && info.size > 0) {
        PushPcm(buf + info.offset, static_cast<size_t>(info.size));
      }
      AMediaCodec_releaseOutputBuffer(audio_codec, (size_t)ob, false);
    }
  };

  auto feed_audio = [&](const uint8_t* data, size_t n) -> bool {
    size_t off = 0;
    int fail = 0;
    while (off < n && !stop) {
      if (++fail > kFeedFailMax) return false;  // 熔断
      const ssize_t ib = AMediaCodec_dequeueInputBuffer(audio_codec, 5000);
      if (ib < 0) {
        drain_audio();
        continue;
      }
      size_t cap = 0;
      uint8_t* in = AMediaCodec_getInputBuffer(audio_codec, (size_t)ib, &cap);
      if (in == nullptr || cap == 0) continue;
      const size_t put = std::min(cap, n - off);
      std::memcpy(in, data + off, put);
      AMediaCodec_queueInputBuffer(audio_codec, (size_t)ib, 0, put, 0, 0);
      off += put;
      drain_audio();
    }
    return off >= n;
  };

  // ---- 主循环：PS 解复用 -----------------------
  while (!stop) {
    if (max_ms > 0 && SteadyMs() - t0 > max_ms) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "mov: 到达 max_ms，停止");
      break;
    }
    if (!ensure(4)) {  // EOF
      if (!loop || stop) break;
      // movLoop(2)：放完从头再来。回到文件开头、清掉解复用状态（解码器保留，
      // 重新喂一遍序列头没问题）。音频源已在引擎里持续播放，这里只是重新喂 ES，
      // 环形缓冲里残留的尾巴最多造成一次听感上的极小重叠（几秒量级）。
      if (::lseek(fd, 0, SEEK_SET) < 0) {
        loop = false;
        break;
      }
      buf.clear();
      pos = 0;
      au.clear();
      au_scan = 0;
      saw_first_picture = false;
      au_audio_hint.clear();
      AppendAppLogLine("mov: 循环重播 " + file_id);
      continue;
    }
    const uint8_t* p = buf.data() + pos;
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) {
      ++pos;
      continue;
    }
    const uint8_t sc = p[3];
    if (sc == 0xBA) {  // pack header（12 字节）
      if (!ensure(12)) break;
      pos += 12;
    } else if (sc == 0xBB || sc == 0xBE || sc == 0xBF) {  // 系统头/填充
      if (!ensure(6)) break;
      const size_t len = (buf[pos + 4] << 8) | buf[pos + 5];
      if (!ensure(6 + len)) break;
      pos += 6 + len;
    } else if (sc >= 0xC0 && sc <= 0xDF) {  // 音频 PES（M4）
      if (!ensure(6)) break;
      const size_t len = (buf[pos + 4] << 8) | buf[pos + 5];
      if (len == 0) {
        pos += 4;
        continue;
      }
      if (!ensure(6 + len)) break;
      size_t q = pos + 6;
      const size_t end = pos + 6 + len;
      while (q < end && buf[q] == 0xFF) ++q;  // pack 层填充
      if (q + 2 < end && (buf[q] & 0xC0) == 0x40) {
        // 音频 PES 头与视频同构：2 字节 +（PTS/DTS 或 1 字节标记）
        const uint8_t nib = (buf[q + 2] >> 4) & 0x0F;
        q += 2 + (nib == 0x2 ? 5 : (nib == 0x3 ? 10 : 1));
      }
      if (q < end) {
        if (audio_codec == nullptr && !audio_failed.load() &&
            au_audio_hint.size() < 4096) {
          au_audio_hint.insert(au_audio_hint.end(), buf.begin() + q,
                               buf.begin() + end);
        }
        if (ensure_audio_codec()) {
          feed_audio(buf.data() + q, end - q);
        }
      }
      pos = end;
    } else if (sc >= 0xE0 && sc <= 0xEF) {  // 视频 PES
      if (!ensure(6)) break;
      const size_t len = (buf[pos + 4] << 8) | buf[pos + 5];
      if (len == 0) {  // 非法/未定长：跳过同步字继续找
        pos += 4;
        continue;
      }
      if (!ensure(6 + len)) break;
      size_t q = pos + 6;
      const size_t end = pos + 6 + len;
      while (q < end && buf[q] == 0xFF) ++q;  // pack 层填充
      if (q + 2 < end && (buf[q] & 0xC0) == 0x40) {
        // MPEG-1 PES 头：2 字节 +（PTS/DTS 或 1 字节标记，见 D-027）
        const uint8_t nib = (buf[q + 2] >> 4) & 0x0F;
        q += 2 + (nib == 0x2 ? 5 : (nib == 0x3 ? 10 : 1));
        if (q < end) {
          au.insert(au.end(), buf.begin() + q, buf.begin() + end);
          // 按 picture 起始码切访问单元
          while (!stop) {
            const size_t i = FindPictureStart(au, au_scan);
            if (i == std::string::npos) {
              au_scan = au.size() > 3 ? au.size() - 3 : 0;
              break;
            }
            if (i == 0) {
              au_scan = 4;
              continue;
            }
            if (!saw_first_picture) {
              // 第一刀切在第二帧的开头：此刻 [0,i) = 序列头 + GOP + 第一帧
              saw_first_picture = true;
              if (!ensure_codec(au.data(), i)) {
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "mov: 解码器建不起来，退出");
                stop = true;
                break;
              }
            }
            if (!codec_started || !feed(au.data(), i)) {
              stop = true;
              break;
            }
            au.erase(au.begin(), au.begin() + i);
            au_scan = 4;
          }
          if (au.size() > kMaxAuBytes) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "mov: 熔断（AU 过大）");
            break;
          }
        }
      }
      pos = end;
    } else {
      pos += 4;
    }
    if (pos > (1u << 20)) {  // 压缩读缓冲
      buf.erase(buf.begin(), buf.begin() + pos);
      pos = 0;
    }
  }

  // ---- 收尾：把最后一段 AU 喂掉、发 EOS、把剩下的帧收干净 ----
  if (!stop.load() && codec_started && !au.empty()) {
    feed(au.data(), au.size());
  }
  if (codec_started && !stop.load() && !eos_sent) {
    const ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 20000);
    if (ib >= 0) {
      AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, 0,
                                   static_cast<uint64_t>(au_index) * 33366ull,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      eos_sent = true;
    }
  }
  for (int i = 0; i < 400 && codec_started && !eos_seen && !stop.load(); ++i) {
    AMediaCodecBufferInfo info;
    const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 5);
    if (ob >= 0) {
      // 复用 drain 逻辑代价高，这里只为把 eos 标志读出来并释放缓冲
      AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) {
        eos_seen = true;
      }
    } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      continue;
    }
  }

  if (codec != nullptr) {
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
  if (fmt != nullptr) AMediaFormat_delete(fmt);
  // 音频收尾：把最后的 PCM 拉干净，再告诉音源「没有更多数据了」——
  // 之后 MovieAudioSource 会在环形缓冲排空后正常收尾。
  if (audio_codec != nullptr) {
    const ssize_t ib = AMediaCodec_dequeueInputBuffer(audio_codec, 20000);
    if (ib >= 0) {
      AMediaCodec_queueInputBuffer(audio_codec, (size_t)ib, 0, 0, 0,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    }
    for (int i = 0; i < 100 && !stop.load(); ++i) {
      drain_audio();
      if (audio_source_raw == nullptr) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    AMediaCodec_stop(audio_codec);
    AMediaCodec_delete(audio_codec);
    audio_codec = nullptr;
  }
  if (audio_fmt != nullptr) {
    AMediaFormat_delete(audio_fmt);
    audio_fmt = nullptr;
  }
  if (audio_source_raw != nullptr) audio_source_raw->MarkEnded();
  ::close(fd);
  finished = true;
  alive = false;
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "mov: 解码线程结束（帧=%lld，丢=%lld）", decoded.load(),
                      dropped);
  AppendAppLogLine("mov: 播放结束 " + file_id + " 解出帧=" +
                   std::to_string(decoded.load()) + " 跳过=" +
                   std::to_string(dropped));
}

}  // namespace rlvm_android
