#include "android/game_file_system.h"

#include <fcntl.h>
#include <unistd.h>

#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "android/saf_file_system.h"

namespace rlvm_android {
namespace {

std::shared_ptr<GameFileSystem> g_file_system;

class PosixGameFileSystem : public GameFileSystem {
 public:
  explicit PosixGameFileSystem(std::string root) : root_(std::move(root)) {}

  bool IsDirectory(const std::string& rel_path) override {
    return boost::filesystem::is_directory(Resolve(rel_path));
  }

  std::vector<DirectoryEntry> ListDirectory(const std::string& rel_path) override {
    std::vector<DirectoryEntry> entries;
    boost::filesystem::directory_iterator end;
    for (boost::filesystem::directory_iterator it(Resolve(rel_path)); it != end;
         ++it) {
      DirectoryEntry entry;
      entry.name = it->path().filename().string();
      entry.is_directory = boost::filesystem::is_directory(it->status());
      entries.push_back(entry);
    }
    return entries;
  }

  std::string MakeId(const std::string& rel_path) override {
    return Resolve(rel_path).string();
  }

  int OpenFd(const std::string& rel_path) override {
    return ::open(Resolve(rel_path).c_str(), O_RDONLY);
  }

 private:
  boost::filesystem::path Resolve(const std::string& rel_path) const {
    if (rel_path.empty()) return boost::filesystem::path(root_);
    boost::filesystem::path path(rel_path);
    // FindFile 在普通路径后端下返回的是绝对路径，直接使用；
    // 只有相对路径才需要拼到游戏根目录上。
    if (path.is_absolute()) return path;
    return boost::filesystem::path(root_) / path;
  }

  std::string root_;
};

class SafGameFileSystem : public GameFileSystem {
 public:
  explicit SafGameFileSystem(std::shared_ptr<SafBackend> backend)
      : backend_(std::move(backend)) {}

  bool IsDirectory(const std::string& rel_path) override {
    return backend_->IsDirectory(rel_path);
  }

  std::vector<DirectoryEntry> ListDirectory(const std::string& rel_path) override {
    std::vector<DirectoryEntry> entries;
    for (const auto& source : backend_->ListDirectory(rel_path)) {
      DirectoryEntry entry;
      entry.name = source.name;
      entry.is_directory = source.is_directory;
      entries.push_back(entry);
    }
    return entries;
  }

  // SAF 下标识就是相对路径本身：对引擎是不透明字符串，只有 OpenFd 认识它。
  std::string MakeId(const std::string& rel_path) override { return rel_path; }

  int OpenFd(const std::string& rel_path) override {
    return backend_->OpenFd(rel_path);
  }

 private:
  std::shared_ptr<SafBackend> backend_;
};

}  // namespace

void SetGameFileSystem(std::shared_ptr<GameFileSystem> file_system) {
  g_file_system = std::move(file_system);
}

std::shared_ptr<GameFileSystem> GetGameFileSystem() { return g_file_system; }

std::shared_ptr<GameFileSystem> MakePosixGameFileSystem(const std::string& root) {
  if (root.empty()) return nullptr;
  boost::filesystem::path path(root);
  if (!boost::filesystem::is_directory(path)) return nullptr;
  return std::make_shared<PosixGameFileSystem>(root);
}

std::shared_ptr<GameFileSystem> MakeSafGameFileSystem() {
  std::shared_ptr<SafBackend> backend = GetSafBackend();
  if (!backend) return nullptr;
  return std::make_shared<SafGameFileSystem>(backend);
}

bool ReadGameFile(const std::string& rel_path, std::vector<char>& out) {
  out.clear();
  std::shared_ptr<GameFileSystem> file_system = GetGameFileSystem();
  if (!file_system) return false;

  const int fd = file_system->OpenFd(rel_path);
  if (fd < 0) return false;

  char buffer[16 * 1024];
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      close(fd);
      out.clear();
      return false;
    }
    if (n == 0) break;
    out.insert(out.end(), buffer, buffer + n);
  }
  close(fd);
  return true;
}

}  // namespace rlvm_android
