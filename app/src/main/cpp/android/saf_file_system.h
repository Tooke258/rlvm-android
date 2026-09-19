// SAF（Storage Access Framework）文件访问门面。
//
// 背景：task.md 硬性要求 Android 12+ 的外部文件访问一律通过 SAF，
// 不得使用 READ_EXTERNAL_STORAGE 之类的宽泛权限，也不得直接拼 File 路径。
// 而 RLVM 上游到处使用 boost::filesystem::path（拼接、exists、目录遍历），
// 两者模型不兼容——这是架构报告里的 R1 风险项。
//
// 设计：native 侧只暴露一个很小的后端接口，由 Kotlin 用 DocumentsContract 实现。
// 所有路径都相对于用户在系统选择器中选定的目录树。SAF 能提供真实 fd，
// 所以大文件（SEEN.TXT）走 fd + mmap，只有小文件才整体读入内存。
//
// 当前接入范围：Gameexe.ini（走 SafReadAll）与 SEEN 归档（走 OpenFd）。
// 图像/音频归档与存档读写的接入放在后续阶段。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_SAF_FILE_SYSTEM_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_SAF_FILE_SYSTEM_H_

#include <memory>
#include <string>
#include <vector>

namespace rlvm_android {

class SafBackend {
 public:
  virtual ~SafBackend() {}

  // 目录项。带上「是否为目录」的标志，避免调用方对每个文件再单独发一次查询——
  // SAF 的每次查询都是一次跨进程调用，真实游戏目录（数千文件）下代价极高。
  struct DirectoryEntry {
    std::string name;
    bool is_directory;
  };

  virtual bool Exists(const std::string& rel_path) = 0;
  virtual bool IsDirectory(const std::string& rel_path) = 0;
  virtual long long Size(const std::string& rel_path) = 0;
  virtual std::vector<DirectoryEntry> ListDirectory(const std::string& rel_path) = 0;

  // 返回只读 fd，调用方负责 close()。失败返回 -1。
  virtual int OpenFd(const std::string& rel_path) = 0;
};

void SetSafBackend(std::shared_ptr<SafBackend> backend);
std::shared_ptr<SafBackend> GetSafBackend();

// 通过 SAF 把整个文件读进内存。文件不存在或读取失败返回 false。
bool SafReadAll(const std::string& rel_path, std::string& out);

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_SAF_FILE_SYSTEM_H_
