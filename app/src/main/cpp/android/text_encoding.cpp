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
    // ASCII（自检工具逮到的漏项：标题里的 "(" ")" 不在表里 → 一处 miss 就整串回退）
    for (int b = 0x20; b <= 0x7E; ++b) add(std::string(1, char(b)));
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
  // 真机实测（D-033，两步死路都踩过）：
  // ① cp932toUnicode(title, encoding) 永远按 CP932 位解（参数只管宽度变换）→ 对 GBK 恒等；
  // ② Cp936::JisEncodeString 输出的是 rlBabel 内部 JIS 表示，不是 CP932 字节形式。
  // 正确路线 = 源编码 → Unicode（Cp936::ConvertString，查 gbk_to_uni[]）→ CP932 字节
  // （本文件下面的 Cp932Table()）。
  const Codepage& cp = Cp::instance(encoding);
  const std::wstring ws = cp.ConvertString(title);
  if (ws.empty()) return title;
  const auto& table = Cp932Table();
  std::string out;
  out.reserve(title.size());
  for (wchar_t wc : ws) {
    const auto it = table.find(static_cast<uint32_t>(wc));
    if (it == table.end()) {
      // CP932 里没有的字（例如汉字简化字）：用 '?' 顶上，别让整串回退成乱码。
      out += '?';
    } else {
      out += it->second;
    }
  }
  return out;
}

std::string RunEncodingSelfTest() {
  std::string out;
  const std::string gbk =
      "\xa3\xb7\xd4\xc2\xa3\xb1\xa3\xb8\xc8\xd5\x28\xd0\xc7\xc6\xda\xc8\xfd\x29";
  const std::string expect =
      "\x82\x56\x8c\x8e\x82\x50\x82\x57\x93\xfa\x28\x90\xaf\x8a\xfa\x8e\x4f\x29";
  auto line = [&out](const std::string& s) { out += s + "\n"; };
  line("enc-selftest: gbk    = " + HexPreview(gbk, 64));
  line("enc-selftest: expect = " + HexPreview(expect, 64));

  // 步 1：源编码 → Unicode
  const Codepage& cp = Cp::instance(1);
  const std::wstring ws = cp.ConvertString(gbk);
  std::string cps;
  for (size_t i = 0; i < ws.size() && i < 12; ++i) {
    char b[16];
    std::snprintf(b, sizeof(b), "U+%04X ", static_cast<unsigned>(ws[i]));
    cps += b;
  }
  line("enc-selftest: step1 ConvertString size=" + std::to_string(ws.size()) +
       " cps=" + (cps.empty() ? std::string("(empty)") : cps));

  // 步 2：Unicode → CP932 字节（查表）
  const std::unordered_map<uint32_t, std::string>& table = Cp932Table();
  std::string hits, misses;
  for (wchar_t wc : ws) {
    const auto it = table.find(static_cast<uint32_t>(static_cast<unsigned short>(wc)));
    if (it == table.end()) {
      char b[16];
      std::snprintf(b, sizeof(b), "U+%04X ", static_cast<unsigned>(wc));
      misses += b;
    } else {
      hits += it->second;
    }
  }
  line("enc-selftest: step2 hits = " + HexPreview(hits, 64) +
       " misses = " + (misses.empty() ? std::string("(none)") : misses));
  line("enc-selftest: table size = " + std::to_string(table.size()));
  const uint32_t probes[] = {0xFF17u, 0x6708u, 0x65E5u, 0x661Fu, 0x671Fu};
  for (uint32_t probe : probes) {
    const auto it = table.find(probe);
    char b[48];
    std::snprintf(b, sizeof(b), "enc-selftest: probe U+%04X -> %s", probe,
                  it == table.end() ? "(miss)" : HexPreview(it->second, 4).c_str());
    line(b);
  }

  // 步 3：完整转换 + 与真值比对
  const std::string got = NormalizeTitleToCP932(gbk, 1);
  line("enc-selftest: total  = " + HexPreview(got, 64) +
       (got == expect ? "   OK" : "   DIFF"));
  return out;
}

}  // namespace rlvm_android
