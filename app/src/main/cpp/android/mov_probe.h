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
// 普通路径后端下就是路径，SAF 后端下是不透明标识。out_dir 非空时把解码出的前几帧
// 转成 PPM 写进去（用于肉眼确认 YUV→RGB 转换是否正确）。返回一段可读的多行报告。
std::string RunMovProbe(const std::string& file_id, const std::string& out_dir,
                        const std::string& codec_name = std::string(),
                        bool set_csd = true);

// 第二条通路（M3a 对照）：**平台自带**的 MPEG-PS 解复用（AMediaExtractor）+
// 平台解码器。path 是设备上可读的绝对路径（诊断时把 .mpg 放到应用外部文件
// 目录），out_dir 非空时把最早几帧导成 PPM。用来回答「是我们自写的 PS 解复用
// 有问题，还是喂法有问题」——真机上系统播放器能正常播这个文件。
std::string RunMovExtractProbe(const std::string& path, const std::string& out_dir);

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_MOV_PROBE_H_
