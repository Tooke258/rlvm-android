// NDK 冒烟测试：验证 C++17、std::filesystem、arm64-v8a 交叉编译链路是否可用。
// 与 RLVM 移植直接相关：R5 风险点就是 boost::filesystem -> std::filesystem 的可行性。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

extern "C" int rlvm_smoke_probe(const char* path) {
  namespace fs = std::filesystem;
  std::vector<std::string> names;
  if (path != nullptr) {
    for (const auto& entry : fs::directory_iterator(path)) {
      names.push_back(entry.path().filename().string());
    }
  }
  int total = static_cast<int>(names.size());
  for (const std::string& n : names) {
    total += static_cast<int>(n.size());
  }
  return total;
}
