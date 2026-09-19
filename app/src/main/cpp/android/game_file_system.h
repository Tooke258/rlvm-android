// 游戏资源文件的查找层抽象。
//
// 上游 System::FindFile 的做法是：读 Gameexe 的 #FOLDNAME 得到合法子目录名，
// 枚举游戏根目录，把匹配到的子目录里的文件建成索引
//（小写主文件名 -> 扩展名 + 文件路径），之后按 basename 与候选扩展名查找。
//
// 这条链路完全建立在 boost::filesystem 之上，在 SAF 下无法工作（没有可枚举的目录）。
// 因此把「判断目录 / 列目录 / 生成文件标识 / 打开文件」四件事抽出来交给后端：
//   - 普通路径后端：标识就是绝对路径，保持上游原有行为不变；
//   - SAF 后端：标识是相对于授权目录树的路径，打开时交给 SafBackend::OpenFd。
//
// 注意：FindFile 的返回值类型仍是 boost::filesystem::path，以避免改动 11 处调用点。
// 普通路径后端下它确实是真实路径；SAF 后端下它只是不透明的文件标识，
// 消费方必须通过 OpenFd 打开，而不能对它做路径运算。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_GAME_FILE_SYSTEM_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_GAME_FILE_SYSTEM_H_

#include <memory>
#include <string>
#include <vector>

namespace rlvm_android {

class GameFileSystem {
 public:
  virtual ~GameFileSystem() {}

  struct DirectoryEntry {
    std::string name;
    bool is_directory;
  };

  virtual bool IsDirectory(const std::string& rel_path) = 0;
  virtual std::vector<DirectoryEntry> ListDirectory(const std::string& rel_path) = 0;

  // 相对路径 -> 文件标识（FindFile 的返回值）。
  virtual std::string MakeId(const std::string& rel_path) = 0;

  // 打开文件用于读取，返回只读 fd，调用方负责 close()。失败返回 -1。
  virtual int OpenFd(const std::string& rel_path) = 0;
};

void SetGameFileSystem(std::shared_ptr<GameFileSystem> file_system);
std::shared_ptr<GameFileSystem> GetGameFileSystem();

// 以普通目录为根。root 为空或不是目录时返回 nullptr。
std::shared_ptr<GameFileSystem> MakePosixGameFileSystem(const std::string& root);

// 以当前 SAF 后端为根（未安装 SAF 后端时返回 nullptr）。
std::shared_ptr<GameFileSystem> MakeSafGameFileSystem();

// 读取游戏目录下的文件到内存（经 GameFileSystem，SAF 下同样成立）。
// rel_path 可以是相对路径；普通路径后端也接受绝对路径。
bool ReadGameFile(const std::string& rel_path, std::vector<char>& out);

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_GAME_FILE_SYSTEM_H_
