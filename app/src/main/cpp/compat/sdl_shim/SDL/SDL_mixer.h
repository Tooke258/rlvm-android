// Minimal SDL / SDL_mixer compatibility shim for the Android port.
//
// 背景：RLVM 的 vendor/xclannad/wavfile.cc（KOE / NWK 语音解码）调用 SDL 1.2 的
// 音频转换 API（SDL_AudioCVT / SDL_BuildAudioCVT / SDL_ConvertAudio）。Android 上
// 没有 SDL，而音频后端将在 T4.1 用 AAudio 自行实现。因此这里只实现 wavfile.cc
// 实际用到的那一小部分：
//   - PCM 格式转换（S8 <-> S16）
//   - 声道数转换（单声道 <-> 立体声）
//   - 采样率线性重采样
//
// 这不是 SDL 的替代品，也不提供任何其它 SDL 功能。等 AAudio 后端成型、能够直接
// 接受源格式之后，本 shim 可以整体删除。
//
// 注意：该转换发生在语音解码路径（主线程）而非音频回调内，因此允许分配内存。

#ifndef RLVM_APP_SRC_MAIN_CPP_COMPAT_SDL_SHIM_SDL_SDL_MIXER_H_
#define RLVM_APP_SRC_MAIN_CPP_COMPAT_SDL_SHIM_SDL_SDL_MIXER_H_

#include <cstdint>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;

// SDL 1.2 的音频格式编码：低 8 位为位宽，bit15 表示有符号。
enum {
  AUDIO_U8 = 0x0008,
  AUDIO_S8 = 0x8008,
  AUDIO_U16LSB = 0x0010,
  AUDIO_S16LSB = 0x8010,
  AUDIO_U16MSB = 0x1010,
  AUDIO_S16MSB = 0x9010
};

// Android 的所有受支持 ABI 都是小端。
#define AUDIO_U16SYS AUDIO_U16LSB
#define AUDIO_S16SYS AUDIO_S16LSB
#define AUDIO_U16 AUDIO_U16SYS
#define AUDIO_S16 AUDIO_S16SYS
#define MIX_DEFAULT_FORMAT AUDIO_S16SYS

// 仅在 wavfile.cc 被 #if 排除的 SMPEG 分支里出现，这里给出完整定义以便编译。
struct SDL_AudioSpec {
  int freq;
  Uint16 format;
  Uint8 channels;
  Uint8 silence;
  Uint16 samples;
  Uint32 size;
};

struct SDL_AudioCVT {
  int needed;
  Uint16 src_format;
  Uint16 dst_format;
  double src_rate;
  double dst_rate;
  Uint8* buf;      // 输入数据；转换结果就地写回此缓冲区
  int len;         // 输入字节数
  int len_cvt;     // 输出字节数（由 SDL_ConvertAudio 填写）
  int len_mult;    // buf 不得小于 len * len_mult
  double len_ratio;

  // 以下两个字段是本 shim 的私有扩展，SDL 原生结构体中没有。
  Uint8 src_channels;
  Uint8 dst_channels;
};

// 返回值遵循 SDL 约定：-1 表示出错，0 表示无需转换，1 表示需要转换。
int SDL_BuildAudioCVT(SDL_AudioCVT* cvt,
                      Uint16 src_format, Uint8 src_channels, int src_rate,
                      Uint16 dst_format, Uint8 dst_channels, int dst_rate);

// 成功返回 0。转换结果写回 cvt->buf，长度写入 cvt->len_cvt。
int SDL_ConvertAudio(SDL_AudioCVT* cvt);

#endif  // RLVM_APP_SRC_MAIN_CPP_COMPAT_SDL_SHIM_SDL_SDL_MIXER_H_
