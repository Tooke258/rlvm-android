#include "android/game_file_system.h"

#include <fcntl.h>
#include <unistd.h>

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
    return boost::filesystem::is_directory(Absolute(rel_path));
  }

  std::vector<std::string> ListDirectory(const std::string& rel_path) override {
    std::vector<std::string> names;
    boost::filesystem::directory_iterator end;
    for (boost::filesystem::directory_iterator it(Absolute(rel_path)); it != end;
         ++it) {
      names.push_back(it->path().filename().string());
    }
    return names;
  }

  std::string MakeId(const std::string& rel_path) override {
    return Absolute(rel_path).string();
  }

  int OpenFd(const std::string& rel_path) override {
    return ::open(Absolute(rel_path).c_str(), O_RDONLY);
  }

 private:
  boost::filesystem::path Absolute(const std::string& rel_path) const {
    if (rel_path.empty()) return boost::filesystem::path(root_);
    return boost::filesystem::path(root_) / rel_path;
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

  std::vector<std::string> ListDirectory(const std::string& rel_path) override {
    return backend_->ListDirectory(rel_path);
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

}  // namespace rlvm_android
