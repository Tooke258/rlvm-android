// 见 SDL/SDL_mixer.h 中的说明。这里实现最小可用的 PCM 转换。

#include "SDL/SDL_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

int BytesPerSample(Uint16 format) { return (format & 0xFF) / 8; }

bool IsSigned(Uint16 format) { return (format & 0x8000) != 0; }

// 把单个采样读成有符号 16 位量级的值。
int32_t LoadSample(const Uint8* p, int bytes, bool is_signed) {
  if (bytes == 1) {
    return is_signed ? (static_cast<int32_t>(static_cast<int8_t>(*p)) << 8)
                     : ((static_cast<int32_t>(*p) - 128) << 8);
  }
  int16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void StoreSample(Uint8* p, int bytes, bool is_signed, int32_t value) {
  const int32_t clamped = std::max(-32768, std::min(32767, value));
  if (bytes == 1) {
    p[0] = is_signed ? static_cast<Uint8>(static_cast<int8_t>(clamped >> 8))
                     : static_cast<Uint8>((clamped >> 8) + 128);
  } else {
    const int16_t v16 = static_cast<int16_t>(clamped);
    std::memcpy(p, &v16, sizeof(v16));
  }
}

// 解码路径为单线程（语音解析在引擎线程上完成），复用缓冲区即可，避免每次分配。
thread_local std::vector<int32_t> g_input;
thread_local std::vector<int32_t> g_output;

}  // namespace

int SDL_BuildAudioCVT(SDL_AudioCVT* cvt,
                      Uint16 src_format, Uint8 src_channels, int src_rate,
                      Uint16 dst_format, Uint8 dst_channels, int dst_rate) {
  if (cvt == nullptr) return -1;
  if (src_channels == 0 || dst_channels == 0) return -1;
  if (src_rate <= 0 || dst_rate <= 0) return -1;
  if (BytesPerSample(src_format) == 0 || BytesPerSample(dst_format) == 0) return -1;

  std::memset(cvt, 0, sizeof(*cvt));
  cvt->src_format = src_format;
  cvt->dst_format = dst_format;
  cvt->src_rate = src_rate;
  cvt->dst_rate = dst_rate;
  cvt->src_channels = src_channels;
  cvt->dst_channels = dst_channels;

  const double ratio = (static_cast<double>(dst_rate) / src_rate) *
                       (static_cast<double>(dst_channels) / src_channels) *
                       (static_cast<double>(BytesPerSample(dst_format)) /
                        BytesPerSample(src_format));
  cvt->len_ratio = ratio;
  // 调用方按 len * len_mult 分配缓冲区，必须留足余量。
  cvt->len_mult = std::max(1, static_cast<int>(std::ceil(ratio)));

  cvt->needed = (src_format != dst_format || src_channels != dst_channels ||
                 src_rate != dst_rate)
                    ? 1
                    : 0;
  return cvt->needed;
}

int SDL_ConvertAudio(SDL_AudioCVT* cvt) {
  if (cvt == nullptr) return -1;
  cvt->len_cvt = 0;
  if (cvt->buf == nullptr || cvt->len <= 0) return 0;

  const int src_bytes = BytesPerSample(cvt->src_format);
  const int dst_bytes = BytesPerSample(cvt->dst_format);
  const bool src_signed = IsSigned(cvt->src_format);
  const bool dst_signed = IsSigned(cvt->dst_format);
  const int src_channels = cvt->src_channels > 0 ? cvt->src_channels : 1;
  const int dst_channels = cvt->dst_channels > 0 ? cvt->dst_channels : 1;

  const int total_in = cvt->len / src_bytes;
  const int frames_in = total_in / src_channels;
  if (frames_in <= 0) return 0;

  // 1) 解码到统一的 int32 表示。
  g_input.resize(static_cast<size_t>(total_in));
  for (int i = 0; i < total_in; ++i) {
    g_input[i] = LoadSample(cvt->buf + static_cast<size_t>(i) * src_bytes, src_bytes,
                            src_signed);
  }

  // 2) 采样率变换（线性插值）。
  const double rate_ratio =
      (cvt->src_rate > 0 && cvt->dst_rate > 0) ? (cvt->dst_rate / cvt->src_rate) : 1.0;
  const int frames_out =
      (rate_ratio == 1.0) ? frames_in
                          : std::max(1, static_cast<int>(frames_in * rate_ratio + 0.5));

  // 3) 声道数变换 + 重采样，一次性产出目标格式。
  const size_t total_out = static_cast<size_t>(frames_out) * dst_channels;
  g_output.resize(total_out);

  for (int f = 0; f < frames_out; ++f) {
    const double src_pos = (rate_ratio == 1.0)
                               ? static_cast<double>(f)
                               : (static_cast<double>(f) / rate_ratio);
    const int i0 = std::min(frames_in - 1, static_cast<int>(src_pos));
    const int i1 = std::min(frames_in - 1, i0 + 1);
    const double frac = src_pos - i0;

    for (int ch = 0; ch < dst_channels; ++ch) {
      // 目标声道映射到源声道：源声道多则以 0 号声道为主。
      const int src_ch = (ch < src_channels) ? ch : 0;
      const int32_t a = g_input[static_cast<size_t>(i0) * src_channels + src_ch];
      const int32_t b = g_input[static_cast<size_t>(i1) * src_channels + src_ch];
      g_output[static_cast<size_t>(f) * dst_channels + ch] =
          static_cast<int32_t>(a + (b - a) * frac);
    }
  }

  // 4) 编码回目标格式。
  for (size_t i = 0; i < total_out; ++i) {
    StoreSample(cvt->buf + i * dst_bytes, dst_bytes, dst_signed, g_output[i]);
  }
  cvt->len_cvt = static_cast<int>(total_out * dst_bytes);
  return 0;
}
