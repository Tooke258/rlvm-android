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

// au 里从 from 起找下一个 picture 起始码（00 00 01 00）。
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

}  // namespace

struct MovPlayer::Impl {
  // ---- 播放参数（引擎线程写，解码线程只读）----
  std::string file_id;
  int x = 0, y = 0, w = 0, h = 0;
  int max_ms = 0;

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
                     int max_ms) {
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
    impl_->queue.clear();
    impl_->has_current = false;
    impl_->clock_start_ms = 0;
    impl_->dropped = 0;
  }
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

  // ---- 主循环：PS 解复用 -----------------------
  while (!stop) {
    if (max_ms > 0 && SteadyMs() - t0 > max_ms) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "mov: 到达 max_ms，停止");
      break;
    }
    if (!ensure(4)) break;  // EOF
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
    } else if (sc >= 0xC0 && sc <= 0xDF) {  // 音频（M4 再处理）
      if (!ensure(6)) break;
      const size_t len = (buf[pos + 4] << 8) | buf[pos + 5];
      if (!ensure(6 + len)) break;
      pos += 6 + len;
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
