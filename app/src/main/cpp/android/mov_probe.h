// MOV 影片解码探针（v0.2.3 / M2）。
//
// 目的：验证「自写 MPEG-PS 解复用 + NDK AMediaCodec(video/mpeg2)」这条路线在本设备上
// 是否成立。只做最小闭环——解复用出视频 ES → 喂解码器 → 打印解出的第一帧的分辨率与
// 像素格式，以及失败时的具体错误。**不参与渲染、不改任何游戏逻辑**。
//
// 背景：上游 RLVM 从未实现 MOV 播放；LBEX 的 MOV/op00.mpg 是 MPEG-1 节目流
// （800x600、约 24fps、223MB，必须流式）。Android 的 MediaExtractor 不解析节目流，
// 所以要自己拆 PS/PES。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PROBE_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PROBE_H_

#include <string>

namespace rlvm_android {

// file_id 是交给 OpenGameFileFd() 的"游戏文件标识"（例如 "MOV/op00.mpg"），
// 普通路径后端下就是路径，SAF 后端下是不透明标识。返回一段可读的多行报告。
std::string RunMovProbe(const std::string& file_id);

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PROBE_H_
