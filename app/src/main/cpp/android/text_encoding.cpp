#include "android/text_encoding.h"

#include <mutex>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "encodings/codepage.h"
#include "utilities/string_utilities.h"

namespace rlvm_android {
namespace {

// Unicode → CP932 字节表：用现成的 cp932toUnicode 反查建一次（不引新依赖）。
const std::unordered_map<uint32_t, std::string>& Cp932Table() {
  static const std::unordered_map<uint32_t, std::string> table = [] {
    std::unordered_map<uint32_t, std::string> t;
    auto add = [&t](const std::string& bytes) {
      const std::wstring ws = cp932toUnicode(bytes, 0);
      if (ws.size() != 1 || ws[0] == 0) return;
      t.emplace(static_cast<uint32_t>(ws[0]), bytes);  // 先到先得
    };
    for (int b = 0xA1; b <= 0xDF; ++b) add(std::string(1, char(b)));  // 半角片假名
    for (int lead = 0x81; lead <= 0xFE; ++lead) {
      for (int trail = 0x40; trail <= 0xFC; ++trail) {
        if (trail == 0x7F) continue;
        add(std::string({static_cast<char>(lead), static_cast<char>(trail)}));
      }
    }
    return t;
  }();
  return table;
}

}  // namespace

std::string HexPreview(const std::string& bytes, size_t max) {
  std::string out;
  const size_t n = bytes.size() < max ? bytes.size() : max;
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(bytes[i]));
    out += buf;
  }
  if (bytes.size() > n) out += "…(+" + std::to_string(bytes.size() - n) + ")";
  return out.empty() ? std::string("(empty)") : out;
}

std::string NormalizeTitleToCP932(const std::string& title, int encoding) {
  if (title.empty() || encoding == 0) return title;  // 已经是 CP932
  // 真机实测（D-033）：cp932toUnicode(title, encoding) 对 GBK 字节是**恒等**（它按
  // CP932 位解，参数只管宽度/字形变换）。真正的 GBK↔JIS 转换在 rlBabel 的代码页设置里：
  // Cp936::JisEncodeString 按 GBK 位读入、写出 JIS(≈CP932) 字节。
  const Codepage& cp = Cp::instance(encoding);
  std::vector<char> buf(title.size() * 2 + 8, 0);
  cp.JisEncodeString(title.c_str(), buf.data(), buf.size() - 1);
  const std::string converted(buf.data());
  if (!converted.empty() && converted != title) return converted;
  const std::wstring ws = cp932toUnicode(title, encoding);
  if (ws.empty()) return title;
  const auto& table = Cp932Table();
  std::string out;
  out.reserve(title.size());
  for (wchar_t wc : ws) {
    const auto it = table.find(static_cast<uint32_t>(wc));
    if (it == table.end()) return title;  // 有字不在 CP932：整串保留原样
    out += it->second;
  }
  return out;
}

}  // namespace rlvm_android
