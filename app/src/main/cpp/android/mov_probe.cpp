#include "android/mov_probe.h"

#include <android/log.h>
#include <fcntl.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "utilities/file.h"

namespace rlvm_android {
namespace {

constexpr char kTag[] = "rlvm-mov";
constexpr size_t kChunk = 1 << 20;  // 每次从 fd 读 1MB
constexpr size_t kHeaderScan = 64;  // 在 PES 包头多少字节内找 ES 起始码

// 报告既累积返回给调用方，也**立即**打到 logcat —— 这样不必等引擎停止就能看到结果。
void Say(std::string& r, const std::string& s) {
  r += s;
  std::string line = s;
  while (!line.empty() && (line.back() == '\n')) line.pop_back();
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s", line.c_str());
}

bool IsVideoEsStartCode(uint8_t c) {
  // MPEG-1 视频 ES 的起始码：0x00..0xAF（picture/slice）、0xB2 user_data、
  // 0xB3 sequence、0xB5 extension、0xB7 seq_end、0xB8 GOP、0xAF picture_end。
  return c <= 0xAF || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB7 ||
         c == 0xB8;
}

std::string Hex16(const uint8_t* p, size_t n) {
  std::string s;
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", p[i]);
    s += buf;
  }
  return s;
}

/**
 * I420（COLOR_FormatYUV420Planar，即 Y 平面 + U 平面 + V 平面）→ RGBA8888。
 *
 * 引擎的像素格式是 0xAABBGGRR（小端内存里就是 R,G,B,A 字节序），这里输出同样的
 * 字节序，方便以后直接喂给 AndroidSurface。色彩按 **BT.601 有限范围**换算
 * （SD/MPEG-1 的标准）。stride/slice_h 由解码器给出（本机是 800 / 608）。
 */
void I420ToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w,
                int h, int stride, int slice_h, std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(w) * h * 4);
  const int cstride = stride / 2;
  for (int j = 0; j < h; ++j) {
    const uint8_t* yrow = y + static_cast<size_t>(j) * stride;
    const uint8_t* urow = u + static_cast<size_t>(j / 2) * cstride;
    const uint8_t* vrow = v + static_cast<size_t>(j / 2) * cstride;
    uint8_t* drow = out.data() + static_cast<size_t>(j) * w * 4;
    for (int i = 0; i < w; ++i) {
      const int c = static_cast<int>(yrow[i]) - 16;
      const int d = static_cast<int>(urow[i / 2]) - 128;
      const int e = static_cast<int>(vrow[i / 2]) - 128;
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
  (void)slice_h;
}

// 把 RGBA 写成 P6 PPM（丢 alpha），供 `view_image` 肉眼确认。
bool WritePpm(const std::string& path, const uint8_t* rgba, int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
  for (int j = 0; j < h; ++j) {
    const uint8_t* s = rgba + static_cast<size_t>(j) * w * 4;
    for (int i = 0; i < w; ++i) {
      row[i * 3 + 0] = s[i * 4 + 0];
      row[i * 3 + 1] = s[i * 4 + 1];
      row[i * 3 + 2] = s[i * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

// 只导 Y 平面（灰度 P6/P5），用来把「亮度结构」和「色度」分开看：
// 如果灰度图结构正常、彩图发绿，问题一定在色度平面的取址/排布。
bool WritePpmGray(const std::string& path, const uint8_t* y, int w, int h,
                  int stride) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  std::fprintf(f, "P5\n%d %d\n255\n", w, h);
  for (int j = 0; j < h; ++j) {
    std::fwrite(y + static_cast<size_t>(j) * stride, 1, static_cast<size_t>(w),
                f);
  }
  std::fclose(f);
  return true;
}

// 单个平面的 均值/极值/零字节占比——判断「平面到底有没有被写进去」。
void PlaneStat(const char* name, const uint8_t* p, size_t n, std::string& out) {
  if (p == nullptr || n == 0) {
    out += std::string(name) + "=(empty) ";
    return;
  }
  unsigned long long sum = 0;
  size_t zeros = 0;
  int mn = 255, mx = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = p[i];
    sum += b;
    if (b == 0) ++zeros;
    if (b < mn) mn = b;
    if (b > mx) mx = b;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s mean=%.1f min=%d max=%d zero=%zu/%zu ",
                name, static_cast<double>(sum) / static_cast<double>(n), mn, mx,
                zeros, n);
  out += buf;
}

}  // namespace

std::string RunMovProbe(const std::string& file_id, const std::string& out_dir,
                        const std::string& codec_name, bool set_csd) {
  std::string r;

  const int fd = OpenGameFileFd(file_id);
  if (fd < 0) {
    Say(r, "mov_probe: 打不开 " + file_id + "（OpenGameFileFd 返回 -1）\n");
    return r;
  }

  // ---- 1) 流式读入，边读边解复用出视频 ES -------------------------------
  std::vector<uint8_t> es;
  std::vector<uint8_t> buf(kChunk);
  size_t total_read = 0;
  size_t pack_cnt = 0, sys_cnt = 0, pad_cnt = 0, audio_pes = 0, video_pes = 0;
  size_t video_pes_skipped = 0;  // 包内 64 字节找不到 ES 起始码的（观察到的填充包）
  std::string first_payload_hex;
  // 跨块的滑窗：简单起见，把上一块末尾 3 字节接到本块前面，避免起始码被切断。
  std::vector<uint8_t> window;
  // 只处理前若干 MB 就够验证：解出第一帧即可。
  constexpr size_t kMaxRead = 24u << 20;

  while (total_read < kMaxRead) {
    const ssize_t n = read(fd, buf.data(), buf.size());
    if (n <= 0) break;
    total_read += static_cast<size_t>(n);
    window.insert(window.end(), buf.begin(), buf.begin() + n);

    size_t pos = 0;
    while (pos + 4 <= window.size()) {
      // 找下一个起始码
      size_t i = pos;
      bool found = false;
      while (i + 4 <= window.size()) {
        if (window[i] == 0 && window[i + 1] == 0 && window[i + 2] == 1) {
          found = true;
          break;
        }
        ++i;
      }
      if (!found) {
        pos = window.size() > 3 ? window.size() - 3 : window.size();
        break;
      }
      const uint8_t sc = window[i + 3];
      if (sc == 0xBA) {  // MPEG-1 pack header：12 字节
        ++pack_cnt;
        pos = i + 12;
      } else if (sc == 0xBB || sc == 0xBE || sc == 0xBF) {  // 长度前缀的非 PES
        if (i + 6 > window.size()) { pos = i; break; }
        const size_t len = (window[i + 4] << 8) | window[i + 5];
        if (sc == 0xBB) ++sys_cnt; else ++pad_cnt;
        pos = i + 6 + len;
      } else if (sc >= 0xE0 && sc <= 0xEF) {  // 视频 PES
        ++video_pes;
        if (i + 6 > window.size()) { pos = i; break; }
        const size_t len = (window[i + 4] << 8) | window[i + 5];
        const size_t end = len ? i + 6 + len : window.size();
        if (end > window.size()) { pos = i; break; }  // 本块内不完整，留给下一轮
        // MPEG-1 节目流的视频 PES：
        //   [pack 层的 0xFF 填充] `61 02 <flags>` [PTS 5B?] [DTS 5B?] <ES 数据>
        // 实测（LBEX op00.mpg 1MB 样本）：466 个视频包全部符合该形状，
        // flags 高位 0x30=PTS+DTS(23 个)、0x20=PTS(45)、0x00=无(398)；
        // 头长 = 2 + (PTS ? 5) + (DTS ? 5)。**注意：绝不能按"包里有没有 ES 起始码"
        // 过滤**——MPEG-1 一帧会被拆到多个包，后续包是纯延续数据，本来就没有起始码。
        size_t p = i + 6;
        // 临时诊断：把前 3 个视频包的实际字节打出来，便于对照样本分析。
        if (video_pes <= 3) {
          char dbg[256];
          std::snprintf(dbg, sizeof(dbg), "mov_probe: [diag] 视频包#%zu off=0x%zx len=%zu 载荷前16=%s",
                        video_pes, (size_t)i, len,
                        Hex16(window.data() + p, std::min<size_t>(16, end - p)).c_str());
          Say(r, std::string(dbg) + "\n");
        }
        while (p < end && window[p] == 0xFF) ++p;  // 跳过 pack 层填充
        // 判据只认"MPEG-1 PES 头首字节高两位 = 01"（`& 0xC0 == 0x40`）——
        // **不要**把第二个字节写死：它是 STD buffer 字段，不同作品的文件不同
        // （LBEX 的 op00.mpg 是 `61 02`，Kud Wafter 的是 `61 39`）。
        if (p + 2 < end && (window[p] & 0xC0) == 0x40) {
          const uint8_t flags = window[p + 2] & 0xF0;
          // **关键**：无 PTS/DTS 的包，第 3 字节（本片恒为 0x0F）仍是包头里的
          // 标记字节，必须跳过——少跳这 1 字节会让整条 ES 从该包起每包错位
          // 1 字节，解码出来就是「只有上半帧、下面全是 0」的残帧。
          // 实测：补上这 1 字节后，解出的 ES 与 `ffmpeg -c copy -f mpeg1video`
          // 抽出的 ES **逐字节完全一致**（86,606,495 字节）。
          p += 2 + (flags == 0x20 ? 5 : (flags == 0x30 ? 10 : 1));
          if (first_payload_hex.empty()) {
            first_payload_hex = Hex16(window.data() + i + 6,
                                      std::min<size_t>(24, end - (i + 6)));
          }
        } else {
          // 形态不符：保守起见整包跳过，并在统计里记账，便于对照。
          ++video_pes_skipped;
          p = end;
        }
        if (p < end) es.insert(es.end(), window.begin() + p, window.begin() + end);
        pos = end;
      } else if (sc >= 0xC0 && sc <= 0xDF) {  // 音频 PES，本次跳过
        ++audio_pes;
        if (i + 6 > window.size()) { pos = i; break; }
        const size_t len = (window[i + 4] << 8) | window[i + 5];
        pos = i + 6 + len;
      } else {
        pos = i + 4;
      }
    }
    if (pos > 0) window.erase(window.begin(), window.begin() + pos);

    // 已经攒够 4MB ES 就够验证了（第一帧只需要很少的数据）
    if (es.size() >= (4u << 20)) break;
  }
  close(fd);

  char line[256];
  std::snprintf(line, sizeof(line),
                "mov_probe: 读入 %zu MB；pack=%zu sys=%zu pad=%zu 视频PES=%zu"
                "（跳过填充包 %zu）音频PES=%zu；ES=%zu 字节",
                total_read >> 20, pack_cnt, sys_cnt, pad_cnt, video_pes,
                video_pes_skipped, audio_pes, es.size());
  Say(r, std::string(line) + "\n");
  Say(r, "mov_probe: 首个视频 PES 载荷前 24 字节 = " + first_payload_hex + "\n");

  if (es.empty()) {
    Say(r, "mov_probe: 没有解出任何 ES 数据，解码验证跳过\n");
    return r;
  }

  // ES 起始码统计：正常应能看到 0xB3(sequence)/0xB8(GOP)/0x00..(picture)
  {
    size_t seq = 0, gop = 0, pic = 0, pic_end = 0;
    for (size_t i = 0; i + 4 <= es.size(); ++i) {
      if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
        const uint8_t c = es[i + 3];
        if (c == 0xB3) ++seq;
        else if (c == 0xB8) ++gop;
        else if (c == 0x00) ++pic;
        else if (c == 0xAF) ++pic_end;
      }
    }
    std::snprintf(line, sizeof(line),
                  "mov_probe: ES 起始码统计 seq=%zu GOP=%zu picture=%zu"
                  " picture_end=%zu\n", seq, gop, pic, pic_end);
    Say(r, line);
  }

  // ---- 2) 交给 AMediaCodec(video/mpeg2) 解码，看能否出帧 ---------------
  AMediaFormat* fmt = AMediaFormat_new();
  AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/mpeg2");

  // MPEG-2 解码器需要 codec-specific data：把视频 ES 里的**序列头**
  // （00 00 01 B3 … 到下一个起始码之前）作为 csd-0 传进去；顺手把宽高也设上
  // （有些实现缺这两项也会 EINVAL）。
  {
    size_t b3 = std::string::npos;
    for (size_t i = 0; i + 4 <= es.size(); ++i) {
      if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0xB3) {
        b3 = i;
        break;
      }
    }
    if (b3 != std::string::npos) {
      size_t end = es.size();
      for (size_t i = b3 + 4; i + 4 <= es.size(); ++i) {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
          end = i;
          break;
        }
      }
      const size_t csd_len = end - b3;
      // mov_csd=0：不给 csd/宽高提示，让解码器自己从码流里认（部分硬解在
      // 给了 MPEG-2 风格的 csd 后会按 MPEG-2 语法解 MPEG-1 流）。
      if (set_csd) AMediaFormat_setBuffer(fmt, "csd-0", es.data() + b3, csd_len);
      // 序列头里读宽高（第 5/6 字节起 12 位宽 + 12 位高）
      if (csd_len >= 8) {
        const uint8_t* p = es.data() + b3;
        const int w = (p[4] << 4) | (p[5] >> 4);
        const int h = ((p[5] & 0x0F) << 8) | p[6];
        if (set_csd && w > 0 && h > 0) {
          AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
          AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
        }
        std::snprintf(line, sizeof(line),
                      "mov_probe: csd-0 = %zu 字节（序列头），宽高 %dx%d\n",
                      csd_len, w, h);
        Say(r, line);
      } else {
        std::snprintf(line, sizeof(line), "mov_probe: csd-0 = %zu 字节\n",
                      csd_len);
        Say(r, line);
      }
    } else {
      Say(r, "mov_probe: ES 里没找到序列头（00 00 01 B3），无法提供 csd-0\n");
    }
  }

  // 指定解码器名（诊断用）：MTK 硬解和 c2.android 软解对同一份 ES 的表现可能
  // 完全不同，用它可以区分「我们的 ES 有问题」还是「硬解挑食」。
  AMediaCodec* codec = codec_name.empty()
                           ? AMediaCodec_createDecoderByType("video/mpeg2")
                           : AMediaCodec_createCodecByName(codec_name.c_str());
  if (!codec_name.empty()) {
    Say(r, "mov_probe: 按名字建解码器 " + codec_name + "\n");
  }
  if (codec == nullptr) {
    Say(r, "mov_probe: AMediaCodec_createDecoderByType(video/mpeg2) 失败（无此解码器）\n");
    AMediaFormat_delete(fmt);
    return r;
  }
  media_status_t st = AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0);
  std::snprintf(line, sizeof(line), "mov_probe: configure -> %d\n", (int)st);
  Say(r, line);
  if (st != AMEDIA_OK) {
    AMediaCodec_delete(codec);
    AMediaFormat_delete(fmt);
    return r;
  }
  st = AMediaCodec_start(codec);
  std::snprintf(line, sizeof(line), "mov_probe: start -> %d\n", (int)st);
  Say(r, line);

  // 喂入 ES（按 8KB 分块），同时尽力抽输出
  size_t fed = 0;
  int frames = 0;
  int first_w = 0, first_h = 0, first_fmt = 0;
  int stride = 0, slice_h = 0;
  const size_t kFeed = 262144;
  constexpr int kWantFrames = 30;  // 多解几帧：看「残帧」是不是每一帧都一样
  constexpr int kPpmFrames = 3;    // 只把前 3 帧导成 PPM（省 IO）
  bool eos_sent = false;
  // 熔断：解码器不再回收输入缓冲时**不能**空转，否则会把引擎线程锁死
  // （实测某时刻起 NdkMediaCodec 开始刷 `sf error code: -38`）。
  int guard = 0, feed_fail = 0;
 constexpr int kGuardMax = 20000;
 constexpr int kFeedFailMax = 200;
  // 把 ES 切成「访问单元」：每个 00 00 01 00（picture 起始码）开一个新单元。
  // MediaCodec 的输入契约是**一个输入缓冲装一个完整访问单元**；按固定字节数
  // 切块喂时 MTK 会吐出**只写了上半部分的残帧**（实测 Y 平面 68% 是 0）。
  // 序列头/GOP 头没有自己的 picture，挂在第一个单元前面一起喂。
  std::vector<std::pair<size_t, size_t>> aus;  // [begin, end)
  {
    std::vector<size_t> pics;
    for (size_t i = 0; i + 4 <= es.size(); ++i) {
      if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0x00) {
        pics.push_back(i);
      }
    }
    if (!pics.empty()) {
      aus.push_back({0, pics.size() > 1 ? pics[1] : es.size()});
      for (size_t k = 1; k < pics.size(); ++k) {
        aus.push_back({pics[k], k + 1 < pics.size() ? pics[k + 1] : es.size()});
      }
    }
    Say(r, "mov_probe: ES 访问单元数 = " + std::to_string(aus.size()) + "\n");
  }
  size_t au_idx = 0;
  const auto au_end = [&]() {
    return aus.empty() ? es.size() : aus[au_idx].second;
  };
  while (fed < es.size() && frames < kWantFrames) {
    if (++guard > kGuardMax) {
      Say(r, "mov_probe: 熔断——迭代到上限，退出解码循环\n");
      break;
    }
    ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 2000);
    if (ib >= 0) {
      feed_fail = 0;
      size_t cap = 0;
      uint8_t* in = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
      if (in != nullptr && cap > 0) {
        // 只喂**到当前访问单元的末尾**为止，绝不跨帧切开。
        const size_t want =
            std::min(std::min(kFeed, cap), au_end() - fed);
        const size_t put = want;
        std::memcpy(in, es.data() + fed, put);
        AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, put,
                                     au_idx * 33333ull, 0);
        if (put < au_end() - fed && put == cap) {
          std::snprintf(line, sizeof(line),
                        "mov_probe: 警告——访问单元 %zu 装不进输入缓冲 %zu，"
                        "被切成多块\n",
                        au_idx, cap);
          Say(r, line);
        }
        fed += put;
        if (fed >= au_end() && au_idx + 1 < aus.size()) {
          ++au_idx;
        }
        if (fed >= es.size() && !eos_sent) {
          // 稍后再送 EOS；先看有没有帧出来
        }
      }
    }
    // 连续喂不进去 = 输入队列不再回收，直接停，别空转。
    if (ib < 0 && ++feed_fail > kFeedFailMax) {
      Say(r, "mov_probe: 熔断——解码器连续拒绝输入缓冲，退出解码循环\n");
      break;
    }
    AMediaCodecBufferInfo info;
    ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000);
    if (ob >= 0) {
      if (first_w == 0) {
        AMediaFormat* ofmt = AMediaCodec_getOutputFormat(codec);
        if (ofmt != nullptr) {
          AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &first_w);
          AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &first_h);
          AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &first_fmt);
          AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_STRIDE, &stride);
          AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_h);
          AMediaFormat_delete(ofmt);
        }
      }
      ++frames;
      // M3a：把前几帧导成 PPM，肉眼确认 YUV→RGB（色彩/朝向/有无绿脸）。
      // 只在 out_dir 非空时做；之后 M3b 直接把同样这段转换接到上屏路径。
      if (frames <= kWantFrames) {
        size_t obuf_size = 0;
        uint8_t* obuf = AMediaCodec_getOutputBuffer(codec, (size_t)ob, &obuf_size);
        const int ew = stride > 0 ? stride : first_w;
        const int eh = slice_h > 0 ? slice_h : first_h;
        const size_t need = static_cast<size_t>(ew) * eh * 3 / 2;
        std::snprintf(line, sizeof(line),
                      "mov_probe: 第 %d 帧 info.size=%d 缓冲=%zu 字节"
                      "（stride=%d slice_h=%d 色彩格式=%d，I420 需 %zu）\n",
                      frames, (int)info.size, obuf_size, stride, slice_h,
                      first_fmt, need);
        Say(r, line);
        std::snprintf(line, sizeof(line),
                      "mov_probe: 第 %d 帧 offset=%d flags=0x%x pts=%lld\n",
                      frames, (int)info.offset, info.flags,
                      (long long)info.presentationTimeUs);
        Say(r, line);
        if (obuf == nullptr) {
          Say(r, "mov_probe: AMediaCodec_getOutputBuffer 返回空，跳过本帧导出\n");
        } else if (first_fmt != 19) {
          // 设备实测是 19（COLOR_FormatYUV420Planar）；其它格式先不猜排布。
          std::snprintf(line, sizeof(line),
                        "mov_probe: 色彩格式 %d 不是 I420(19)，M3a 不转换\n",
                        first_fmt);
          Say(r, line);
        } else if (ew <= 0 || eh <= 0 || first_w <= 0 || first_h <= 0 ||
                   obuf_size < need) {
          std::snprintf(line, sizeof(line),
                        "mov_probe: 帧缓冲尺寸不合理（缓冲=%zu < 需 %zu），"
                        "跳过本帧导出\n",
                        obuf_size, need);
          Say(r, line);
        } else {
          const uint8_t* yplane = obuf;
          const uint8_t* uplane = obuf + static_cast<size_t>(ew) * eh;
          const uint8_t* vplane =
              uplane + static_cast<size_t>(ew / 2) * (eh / 2);
          std::string stats;
          PlaneStat("Y", yplane, static_cast<size_t>(ew) * eh, stats);
          PlaneStat("U", uplane, static_cast<size_t>(ew / 2) * (eh / 2), stats);
          PlaneStat("V", vplane, static_cast<size_t>(ew / 2) * (eh / 2), stats);
          Say(r, "mov_probe: 第 " + std::to_string(frames) + " 帧平面统计 " +
                     stats + "\n");
          // 只把前几帧导成 PPM/PGM（省 IO）；统计每帧都打。
          if (frames <= kPpmFrames && !out_dir.empty()) {
          std::vector<uint8_t> rgba;
          I420ToRgba(yplane, uplane, vplane,
                     first_w, first_h, ew, eh, rgba);
          char path[512];
          std::snprintf(path, sizeof(path), "%s/mov-frame-%02d.ppm",
                        out_dir.c_str(), frames);
          const bool ok = WritePpm(path, rgba.data(), first_w, first_h);
          char gpath[512];
          std::snprintf(gpath, sizeof(gpath), "%s/mov-frame-%02d-y.pgm",
                        out_dir.c_str(), frames);
          const bool gok =
              WritePpmGray(gpath, yplane, first_w, first_h, ew);
          std::snprintf(line, sizeof(line),
                        "mov_probe: %s写 PPM %s（%dx%d）%s写灰度 %s\n",
                        ok ? "已" : "未能", path, first_w, first_h,
                        gok ? "已" : "未能", gpath);
          Say(r, line);
          }
        }
      }
      AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
    } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* ofmt = AMediaCodec_getOutputFormat(codec);
      if (ofmt != nullptr) {
        AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_WIDTH, &first_w);
        AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_HEIGHT, &first_h);
        AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &first_fmt);
        AMediaFormat_delete(ofmt);
      }
      Say(r, "mov_probe: 输出格式已确定\n");
    } else if (ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      // 忽略
    } else {
      // TRY_AGAIN 之类：数据还不够，继续喂
    }
  }

  std::snprintf(line, sizeof(line),
                "mov_probe: 已喂入 %zu/%zu 字节；解出帧数=%d；首帧 %dx%d 色彩格式=%d"
                " (0x%x)\n",
                fed, es.size(), frames, first_w, first_h, first_fmt,
                first_fmt);
  Say(r, line);
  std::snprintf(line, sizeof(line),
                "mov_probe: 循环迭代=%d 连续喂失败=%d（上限 %d/%d）\n", guard,
                feed_fail, kGuardMax, kFeedFailMax);
  Say(r, line);
  Say(r, frames > 0
             ? "mov_probe: 结论 = 解码通路可行（AMediaCodec 能解出帧）\n"
             : "mov_probe: 结论 = 未解出帧（需排查 ES 形态或换解码器）\n");

  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  AMediaFormat_delete(fmt);
  return r;
}

/**
 * M3a 对照通路：用**平台的** AMediaExtractor 解 MPEG-PS，再用平台解码器解码。
 *
 * 为什么值得测：真机上系统播放器能把这个 op00.mpg 放成正常彩色 OP，说明
 * 「平台解复用 + 平台解码器」这条链是通的；而我们自写 PS 解复用 + 手工喂 ES
 * 解出来的是残帧。这个探针把两件事分开：
 *   - 平台解复用器认不认这个文件（轨道、mime、时长、csd）；
 *   - 用平台给出的**样本边界**喂解码器，画面是否正常。
 * 如果都正常，v0.2.3 就应该改成走 AMediaExtractor（SAF 的 fd 也能喂），
 * 而不是继续维护手写的 PS 解复用。
 */
std::string RunMovExtractProbe(const std::string& path, const std::string& out_dir) {
  std::string r;
  char line[512];

  AMediaExtractor* ex = AMediaExtractor_new();
  // 路径式 setDataSource 会让**解复用服务进程**去开文件；我们 App 私有目录
  // （Android/data/<pkg>/files）别的 uid 打不开，所以一律返回 -10002。
  // 改成自己开 fd 再交出去——这也是将来接 SAF（拿到的就是 fd）的正确姿势。
  int fd = ::open(path.c_str(), O_RDONLY);
  struct stat stbuf;
  media_status_t st = AMEDIA_ERROR_UNKNOWN;
  if (fd >= 0 && ::fstat(fd, &stbuf) == 0) {
    st = AMediaExtractor_setDataSourceFd(ex, fd, 0, stbuf.st_size);
  }
  std::snprintf(line, sizeof(line), "mov_extract: setDataSource(%s) -> %d\n",
                path.c_str(), (int)st);
  Say(r, line);
  std::snprintf(line, sizeof(line), "mov_extract: open->%d size=%lld\n", fd,
                fd >= 0 ? (long long)stbuf.st_size : 0LL);
  Say(r, line);
  if (st != AMEDIA_OK) {
    if (fd >= 0) ::close(fd);
    AMediaExtractor_delete(ex);
    return r;
  }

  const size_t ntracks = AMediaExtractor_getTrackCount(ex);
  std::snprintf(line, sizeof(line), "mov_extract: 轨道数 = %zu\n", ntracks);
  Say(r, line);

  int video_track = -1;
  for (size_t i = 0; i < ntracks; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
    if (f == nullptr) continue;
    const char* mime = nullptr;
    AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
    int w = 0, h = 0, sr = 0, ch = 0;
    int64_t dur = 0;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
    AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &dur);
    std::snprintf(line, sizeof(line),
                  "mov_extract: 轨道%zu mime=%s %dx%d %dHz %dch 时长=%lldus\n",
                  i, mime != nullptr ? mime : "(null)", w, h, sr, ch,
                  (long long)dur);
    Say(r, line);
    if (video_track < 0 && mime != nullptr &&
        std::strncmp(mime, "video/", 6) == 0) {
      video_track = (int)i;
    }
    AMediaFormat_delete(f);
  }
 if (video_track < 0) {
    if (fd >= 0) ::close(fd);
    Say(r, "mov_extract: 没有视频轨道（平台解复用器不认这个文件？）\n");
    AMediaExtractor_delete(ex);
    return r;
  }

  AMediaExtractor_selectTrack(ex, (size_t)video_track);
  AMediaFormat* vf = AMediaExtractor_getTrackFormat(ex, (size_t)video_track);
  const char* vmime = nullptr;
  AMediaFormat_getString(vf, AMEDIAFORMAT_KEY_MIME, &vmime);
  const std::string mime_str = vmime != nullptr ? vmime : "";
  {
    // csd-0 有没有内容，是判断解复用器是否理解这类流的关键证据。
    size_t csd_len = 0;
    void* csd = nullptr;
    if (!AMediaFormat_getBuffer(vf, "csd-0", &csd, &csd_len)) csd_len = 0;
    std::snprintf(line, sizeof(line),
                  "mov_extract: 选中视频轨道 mime=%s csd-0=%zu 字节\n",
                  mime_str.c_str(), csd_len);
    Say(r, line);
    if (csd_len >= 8 && csd != nullptr) {
      const uint8_t* p = static_cast<const uint8_t*>(csd);
      std::snprintf(line, sizeof(line), "mov_extract: csd-0 头 16 字节 = %s\n",
                    Hex16(p, std::min<size_t>(16, csd_len)).c_str());
      Say(r, line);
    }
  }

 AMediaCodec* codec = AMediaCodec_createDecoderByType(mime_str.c_str());
 if (codec == nullptr) {
    if (fd >= 0) ::close(fd);
    Say(r, "mov_extract: 建解码器失败\n");
    AMediaFormat_delete(vf);
    AMediaExtractor_delete(ex);
    return r;
  }
  st = AMediaCodec_configure(codec, vf, nullptr, nullptr, 0);
  std::snprintf(line, sizeof(line), "mov_extract: configure -> %d\n", (int)st);
  Say(r, line);
  if (st == AMEDIA_OK) st = AMediaCodec_start(codec);
  std::snprintf(line, sizeof(line), "mov_extract: start -> %d\n", (int)st);
  Say(r, line);
  AMediaFormat_delete(vf);

  // 每隔 kEvery 帧导一帧（看运动），最多导 kMaxOut 帧。
  constexpr int kEvery = 30;
  constexpr int kMaxOut = 6;
  constexpr size_t kMaxSamples = 600;  // 熔断：最多读这么多样本
  int samples = 0, frames = 0, exported = 0;
  int iter = 0;
  size_t fed_bytes = 0;
  bool eos_sent = false;
  while (st == AMEDIA_OK && samples < (int)kMaxSamples &&
         exported < kMaxOut && ++iter < 5000) {
    // ---- 喂一个样本（平台解复用器给出的样本就是一个访问单元）----
    const ssize_t ssize = AMediaExtractor_getSampleSize(ex);
    ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 2000);
    if (ib >= 0) {
      size_t cap = 0;
      uint8_t* in = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
      if (in != nullptr && cap > 0) {
        if (ssize <= 0) {
          AMediaCodec_queueInputBuffer(
              codec, (size_t)ib, 0, 0, 0,
              AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          eos_sent = true;
        } else {
          const ssize_t n =
              AMediaExtractor_readSampleData(ex, in, std::min(cap, (size_t)ssize));
          const int64_t pts = AMediaExtractor_getSampleTime(ex);
          AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0,
                                       n > 0 ? (size_t)n : 0, pts, 0);
          fed_bytes += (size_t)(n > 0 ? n : 0);
          ++samples;
          AMediaExtractor_advance(ex);
        }
      }
    }

    // ---- 拉输出 ----
    AMediaCodecBufferInfo info;
    const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
    if (ob >= 0) {
      ++frames;
      if (!out_dir.empty() && frames % kEvery == 1 && exported < kMaxOut) {
        int w = 0, h = 0, cf = 0, stride = 0, slice_h = 0;
        AMediaFormat* of = AMediaCodec_getOutputFormat(codec);
        if (of != nullptr) {
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride);
          AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_h);
          AMediaFormat_delete(of);
        }
        size_t obuf_size = 0;
        uint8_t* obuf =
            AMediaCodec_getOutputBuffer(codec, (size_t)ob, &obuf_size);
        const int ew = stride > 0 ? stride : w;
        const int eh = slice_h > 0 ? slice_h : h;
        if (obuf != nullptr && cf == 19 && w > 0 && h > 0 && ew > 0 && eh > 0 &&
            obuf_size >= static_cast<size_t>(ew) * eh * 3 / 2) {
          std::string stats;
          PlaneStat("Y", obuf, static_cast<size_t>(ew) * eh, stats);
          PlaneStat("U", obuf + static_cast<size_t>(ew) * eh,
                    static_cast<size_t>(ew / 2) * (eh / 2), stats);
          Say(r, "mov_extract: 第 " + std::to_string(frames) + " 帧平面统计 " +
                     stats + "\n");
          std::vector<uint8_t> rgba;
          I420ToRgba(obuf, obuf + static_cast<size_t>(ew) * eh,
                     obuf + static_cast<size_t>(ew) * eh +
                         static_cast<size_t>(ew / 2) * (eh / 2),
                     w, h, ew, eh, rgba);
          ++exported;
          char path2[512];
          std::snprintf(path2, sizeof(path2), "%s/ext-frame-%02d.ppm",
                        out_dir.c_str(), exported);
          const bool ok = WritePpm(path2, rgba.data(), w, h);
          std::snprintf(line, sizeof(line),
                        "mov_extract: %s写 PPM %s（第 %d 帧，%dx%d）\n",
                        ok ? "已" : "未能", path2, frames, w, h);
          Say(r, line);
        } else {
          std::snprintf(line, sizeof(line),
                        "mov_extract: 第 %d 帧没导出（cf=%d %dx%d stride=%d "
                        "slice_h=%d 缓冲=%zu）\n",
                        frames, cf, w, h, stride, slice_h, obuf_size);
          Say(r, line);
        }
      }
      AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0) break;
    } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      Say(r, "mov_extract: 输出格式已确定\n");
    }
    if (ssize < 0 && eos_sent && ib < 0 && ob < 0) {
      Say(r, "mov_extract: 样本与输出都空了，退出\n");
      break;
    }
  }

  std::snprintf(line, sizeof(line),
                "mov_extract: 读样本=%d 喂入=%zu 字节 解出帧=%d 导出=%d\n",
                samples, fed_bytes, frames, exported);
  Say(r, line);
  Say(r, exported > 0 ? "mov_extract: 结论 = 平台解复用 + 平台解码可用\n"
                      : "mov_extract: 结论 = 未能导出画面（看上面的轨道信息）\n");

 AMediaCodec_stop(codec);
 AMediaCodec_delete(codec);
 AMediaExtractor_delete(ex);
  if (fd >= 0) ::close(fd);
  return r;
}

}  // namespace rlvm_android
