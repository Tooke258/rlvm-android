#include "android/mov_probe.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
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

}  // namespace

std::string RunMovProbe(const std::string& file_id) {
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
          p += 2 + (flags == 0x20 ? 5 : (flags == 0x30 ? 10 : 0));
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
      AMediaFormat_setBuffer(fmt, "csd-0", es.data() + b3, csd_len);
      // 序列头里读宽高（第 5/6 字节起 12 位宽 + 12 位高）
      if (csd_len >= 8) {
        const uint8_t* p = es.data() + b3;
        const int w = (p[4] << 4) | (p[5] >> 4);
        const int h = ((p[5] & 0x0F) << 8) | p[6];
        if (w > 0 && h > 0) {
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

  AMediaCodec* codec = AMediaCodec_createDecoderByType("video/mpeg2");
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
  const size_t kFeed = 8192;
  bool eos_sent = false;
  while (fed < es.size() && frames == 0) {
    ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 2000);
    if (ib >= 0) {
      size_t cap = 0;
      uint8_t* in = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
      if (in != nullptr && cap > 0) {
        const size_t want = std::min(kFeed, es.size() - fed);
        const size_t put = std::min(want, cap);
        std::memcpy(in, es.data() + fed, put);
        AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, put,
                                     fed * 1000000ull / 2000000ull, 0);
        fed += put;
        if (fed >= es.size() && !eos_sent) {
          // 稍后再送 EOS；先看有没有帧出来
        }
      }
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
          AMediaFormat_delete(ofmt);
        }
      }
      ++frames;
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
  Say(r, frames > 0
             ? "mov_probe: 结论 = 解码通路可行（AMediaCodec 能解出帧）\n"
             : "mov_probe: 结论 = 未解出帧（需排查 ES 形态或换解码器）\n");

  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  AMediaFormat_delete(fmt);
  return r;
}

}  // namespace rlvm_android
