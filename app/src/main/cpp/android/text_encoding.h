// 文本编码归一化（v0.2.3 / D-033）
#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_TEXT_ENCODING_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_TEXT_ENCODING_H_

#include <string>

namespace rlvm_android {

// 把 title 从 encoding（0=CP932，其它见 Cp::instance）转成 CP932 字节。
// 汉化包的对话场景是 GBK、菜单/LOAD 场景仍是原版 CP932，而存档标题取自当前场景的
// 窗口副标题——不转就会在 LOAD 列表里按 CP932 解成半角片假名（D-033）。
// 转不了（有字不在 CP932）时原样返回，宁可乱码也不丢字。
std::string NormalizeTitleToCP932(const std::string& title, int encoding);

// 诊断用：把字节串前 max 个字节打成十六进制（D-033 复盘用）。
std::string HexPreview(const std::string& bytes, size_t max = 24);

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_TEXT_ENCODING_H_
