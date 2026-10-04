// JNI 桥接层。
//
// task.md 硬性约束：所有 native 方法必须用 RegisterNatives 显式注册，不使用按符号名的动态查找。
// 因此本文件不声明 Java_org_rlvm_android_* 形式的导出函数，而是统一在 JNI_OnLoad 中绑定。

#include <jni.h>

#include <android/log.h>

#include <exception>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <typeinfo>
#include <vector>

#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

// Boost 1.92 起 path.hpp / operations.hpp 不再传递包含 directory.hpp，必须显式引入。
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libreallive/archive.h"
#include "libreallive/bytecode.h"
#include "libreallive/gameexe.h"
#include "libreallive/intmemref.h"
#include "libreallive/scenario.h"
#include "android/android_system.h"
#include "android/android_graphics.h"
#include "android/audio_engine.h"
#include "android/game_file_system.h"
#include "android/jni_saf_backend.h"
#include "android/log_redirect.h"
#include "android/saf_file_system.h"
#include "machine/game_hacks.h"
#include "machine/long_operation.h"
#include "machine/rlmachine.h"
#include "machine/serialization.h"
#include "modules/modules.h"
#include "utilities/file.h"
#include "utilities/string_utilities.h"
#include "utf8cpp/utf8.h"

namespace {

constexpr char kLogTag[] = "rlvm-native";

// ---------------------------------------------------------------------------
// 帧呈现缓冲
//
// 引擎在自己的线程上合成帧，GL 线程按自己的节奏取走最新的一帧。
// 这里保存一份拷贝而不是直接暴露 AndroidGraphicsSystem 的帧缓冲：
// 引擎实例的生命周期（T2.4）尚未定型，拷贝可以让两边解耦。
// ---------------------------------------------------------------------------
std::mutex g_frame_mutex;
std::vector<uint32_t> g_frame_pixels;
int g_frame_width = 0;
int g_frame_height = 0;
unsigned int g_frame_serial = 0;
int g_frame_log_every = 1;

// ---------------------------------------------------------------------------
// 设备侧诊断开关
//
// 为什么需要：真机迭代一轮要重新构建 + 安装（数分钟），而「首屏为什么是黑的」
// 这类问题需要反复调整观察参数（跑多久、要不要逐条指令追踪）。
// 因此把参数放进一个纯文本文件，用 adb push 改一行就能换一次实验，
// 不必重新编译。文件由 Kotlin 侧告知路径（本机外部文件目录）。
//
// 文件格式：每行 `key=value`，`#` 开头为注释。缺省值即原行为。
//   trace=1              逐条指令追踪（上游 set_tracing_on，输出走 stderr）
//   time_budget_ms=8000  单次运行的执行时间片
//   max_instructions=N   指令条数上限
//   frame_log_every=60   每 N 帧才打一条帧日志（默认 1，即每帧）
  //   lb_minigame=1        不跳过 LB/LBEX 的棒球小游戏（逆向 PT00 用；默认 0 = 跳过）
  //   dirty_gate=0         退回「每轮无条件合帧」的旧行为（默认 1 = 脏标记驱动，见 D-022）
  //   dirty_stats=1        每秒打印一次「谁把屏幕标脏」的分类计数（诊断帧率用）
  //   slice_ms=N           主循环时间片（默认 6ms；上游是 10ms。见 D-023）
// ---------------------------------------------------------------------------
std::mutex g_diag_mutex;
std::string g_diag_dir;

// 「停止引擎」请求。由 UI 线程置位，引擎线程在每轮循环开头检查。
// 应用默认持续运行（见 DiagOptions::time_budget_ms 的 0 = 不限时），
// 因此必须有一个从外部喊停的通道。
std::atomic<bool> g_stop_requested{false};

// 「挂起引擎」请求：黑屏 / 应用进后台时由 UI 线程置位，引擎线程在每轮循环开头检查。
//
// 之前没有这条通道：屏幕熄灭后 GL 线程被 onPause 停住，引擎线程却照样按 10ms 时间片
// 推进字节码、音频回调照样出声——用户看到的就是「黑屏之后游戏还在跑」。挂起期间既
// 不推进指令也不合成新帧，音频同时被打成静音并停掉数据回调；置回 false 后从原位置继续。
std::atomic<bool> g_engine_suspended{false};

// 文本容器（例如合并了汉化的 SEEN 归档）在游戏目录树内的相对路径。
// 空 = 用游戏目录里的 Seen.txt。UI 线程写入、引擎线程读取，所以加锁。
std::mutex g_text_container_mutex;
std::string g_text_container_path;

// ---------------------------------------------------------------------------
// 内存探针（诊断用）
//
// 背景：真机上出现过「引擎解析到一半内存暴涨，被系统 lowmemorykiller SIGKILL，
// 连带把系统文件管理器一起杀掉」。为了定位是哪一步在涨内存，这里读 /proc/self/status
// 的 VmRSS 并打日志；同时给一个自身上限，超过就主动停手，避免再把整机拖下水。
// ---------------------------------------------------------------------------
constexpr long kMemoryGuardKb = 1200 * 1024;  // 1.2 GB

long CurrentRssKb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 6, "VmRSS:") != 0) continue;
    try {
      return std::stol(line.substr(6));
    } catch (...) {
      return -1;
    }
  }
  return -1;
}

void LogMemory(const char* phase) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "mem[%s] rss=%ld kB", phase,
                      CurrentRssKb());
}

// 是否已有一台引擎在跑。默认不限时运行后，重复点「运行」很容易起第二台，
// 两台引擎共用同一个 AudioEngine 与帧缓冲会互相踩踏，所以直接拒绝并存。
std::atomic<bool> g_run_active{false};

/**
 * 规整"游戏文件标识"。
 *
 * SAF 模式下我们把 gameexe 的 __GAMEPATH 设成了 "saf:/"，而引擎会用它拼出
 * 存档/全局数据的完整路径（例如 "saf://SAVEDATA/save001.sav"）。这个前缀对
 * SAF 后端没有意义——它要的是相对路径（"SAVEDATA/save001.sav"）。
 * 不剥掉就会去"创建名为 saf: 的目录"从而失败，表现正是"存档/Config 不保留"。
 */
std::string NormalizeGameFileId(const char* file_id) {
  std::string id(file_id);
  const std::string prefix = "saf:";
  // 只有确实带 saf: 前缀时才剥掉它**以及**紧随其后的斜杠；普通绝对路径
  //（例如 HOME 下的存档路径 /storage/...）必须原样保留开头的斜杠，
  // 否则会被当成 SAF 相对路径——上一版就是这样把存档写错了地方。
  if (id.compare(0, prefix.size(), prefix) == 0) {
    id.erase(0, prefix.size());
    while (!id.empty() && id[0] == '/') id.erase(0, 1);
  }
  return id;
}

/**
 * 平台钩子实现：把「游戏文件标识」换成只读 fd。
 *
 * 语音归档（KOE/NWK/OVK/koepac）沿用上游的 fopen/ifstream 按路径读文件，
 * 这在 SAF 下必然失败（没有真实路径）。上游在 utilities/file.h 里留了钩子，
 * 这里实现它：普通路径后端返回真实 fd，SAF 后端由 Kotlin 侧的 ContentResolver
 * 打开同一个文档，两者对调用方没有区别。
 */
int OpenGameFileFdHookImpl(const char* file_id) {
  if (file_id == nullptr) return -1;
  const std::string id = NormalizeGameFileId(file_id);
  // 绝对路径（例如 HOME 指向的应用目录）：直接走 posix，不属于 SAF 树。
  if (!id.empty() && id[0] == '/') return ::open(id.c_str(), O_RDONLY);
  std::shared_ptr<rlvm_android::GameFileSystem> files =
      rlvm_android::GetGameFileSystem();
  if (!files) return -1;
  return files->OpenFd(id);
}

/** 写入变体：存档与全局数据（Config）经这里落盘，SAF 下由 Kotlin 建文档。 */
int OpenGameFileWriteFdHookImpl(const char* file_id) {
  if (file_id == nullptr) return -1;
  std::shared_ptr<rlvm_android::GameFileSystem> files =
      rlvm_android::GetGameFileSystem();
  if (!files) return -1;
  const std::string id = NormalizeGameFileId(file_id);
  // 绝对路径：存档/全局数据可能落在 HOME（应用外部目录）下，那里是真实路径，
  // 不需要也不能走 SAF。父目录不存在时先创建。
  if (!id.empty() && id[0] == '/') {
    const size_t slash = id.find_last_of('/');
    if (slash != std::string::npos) ::mkdir(id.substr(0, slash).c_str(), 0755);
    const int posix_fd = ::open(id.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    __android_log_print(posix_fd >= 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kLogTag,
                        "write open (posix): %s (fd=%d)", id.c_str(), posix_fd);
    return posix_fd;
  }
  const int fd = files->OpenWriteFd(id);
  if (fd >= 0) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "write open: %s (fd=%d)", id.c_str(), fd);
  } else {
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "write open failed: %s", id.c_str());
  }
  return fd;
}

// 当前正在运行的 AndroidSystem。UI 线程的触摸事件需要它才能找到事件系统；
// 引擎停止后清空，避免触到已析构的对象。
std::atomic<AndroidSystem*> g_current_system{nullptr};

// 一次触摸等价于哪个鼠标键（由 diag 的 touch_button 决定，运行期只读）。
std::atomic<int> g_touch_buttons{1};

// 触摸事件的动作码，必须与 NativeBridge.touchEvent 的调用方一致。
constexpr int kTouchDown = 0;
constexpr int kTouchMove = 1;
constexpr int kTouchUp = 2;

/** 作用域内的「唯一运行者」守卫；未取得时 acquired() 为 false。 */
class RunGuard {
 public:
  RunGuard() : acquired_(!g_run_active.exchange(true)) {}
  ~RunGuard() {
    if (acquired_) g_run_active.store(false);
  }
  RunGuard(const RunGuard&) = delete;
  RunGuard& operator=(const RunGuard&) = delete;
  bool acquired() const { return acquired_; }

 private:
  bool acquired_;
};

/** 让 UI 线程能找到当前引擎的事件系统；离开作用域即断开。 */
class CurrentSystemGuard {
 public:
  explicit CurrentSystemGuard(AndroidSystem* system) {
    g_current_system.store(system);
  }
  ~CurrentSystemGuard() { g_current_system.store(nullptr); }
  CurrentSystemGuard(const CurrentSystemGuard&) = delete;
  CurrentSystemGuard& operator=(const CurrentSystemGuard&) = delete;
};

struct DiagOptions {
  bool trace = false;
  bool dump_graphics = false;
  bool audio_selftest = false;
  // 合成统计（逐像素累加）默认关闭，避免拖慢渲染。
  bool blit_stats = false;
  // 汉化用：把 SEEN.TXT 每个场景的文本串按顺序导出（见 docs/LOCALIZATION.md）。
  bool export_jp_text = false;
  // 一次触摸等价于哪个鼠标键（位掩码：1=左键 2=右键 3=两者）。
  // 不同 RealLive 作品的脚本约定不一致，因此做成设备侧可调。
  int touch_button = 1;
  // 逆向 PT00（LB/LBEX 棒球小游戏）用：置 1 则不跳过小游戏，脚本会跑进去并调用
  // PT00.dll，`little_busters_pt00dll.cc` 会把每次 (func, 参数) 打出来。
  bool lb_minigame = false;
  // 合帧策略（D-022）：true = 与上游 SDL 后端一致，只在屏幕被标记为脏时合成一帧；
  // false = 退回旧行为（每轮无条件合成）。做成开关便于真机 A/B 对比。
  bool dirty_gate = true;
  // 每秒打印脏标记的分类来源（dc0/hik/obj/text/mouse），用于判断"合帧率低"是
  // 游戏本来就没标脏，还是我们漏标了。
  bool dirty_stats = false;
  // 0 表示不限时：应用要能一直停在标题/正文上，BGM 才不会「响一下就没了」。
  // 自动化测试需要在报告里拿到结果时，用 diag 文件设一个有限值。
  int time_budget_ms = 0;
  int max_instructions = 0;  // 0 表示沿用调用方传入的值
  int frame_log_every = 1;
  // 主循环每一轮的时间片（毫秒）。上游是 10ms（≈100 轮/秒），但手机屏幕多是
  // 120Hz：100 与 120 不是整数比（5:6），画面会以"1 个 vsync / 2 个 vsync"交替
  // 显示，观感就是抖。把时间片压到 6ms（≈150 轮/秒 > 120Hz）让显示端每个 vsync
  // 都能拿到新帧（见 D-023）。可用诊断文件 slice_ms=N 调回去做对比。
  int slice_ms = 6;
  // 内容每变化一次就导出一张 PPM（最多这么多张）。用来直接"看"动画的形态：
  // 是逐帧图片切换，还是连续位移/渐变——不靠推理。
  int dump_frames = 0;
  // 是否忽略 force_wait（见主循环里的说明）。默认 true：force_wait 是上游 SDL
  // 后端的节奏控制，在我们的架构下只会把脚本循环切碎。写 0 可退回旧行为对比。
  bool exec_ignore_force_wait = true;
  // 诊断：把指定场景号（逗号分隔）的字节码反汇编成源码形式，写进诊断目录。
  // 用于逆向系统脚本（存档/读档菜单）到底调了哪些指令。默认空 = 不导出。
  std::vector<int> dump_scenarios;
  // 诊断：dump_scenario=all 时把整库所有场景反汇编到一个文件（量大，按需开）。
  bool dump_all_scenarios = false;
  // 诊断：主循环解剖。每秒把「轮数 / 字节码指令 / long op 步进 / 栈顶 long op 类型」
  // 分开计数打印——用来回答"每轮主循环为什么只跑得动 1 条指令"（见 D-023 后续）。
  bool loop_probe = false;
};

/** 读取并解析诊断文件；文件不存在时返回缺省值。 */
DiagOptions LoadDiagOptions() {
  DiagOptions options;

  std::string dir;
  {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    dir = g_diag_dir;
  }
  if (dir.empty()) return options;

  std::ifstream stream(dir + "/rlvm-diag.txt");
  if (!stream) return options;

  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::string::size_type equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    const int number = std::atoi(value.c_str());
    if (key == "trace") {
      options.trace = (number != 0);
    } else if (key == "dump_graphics") {
      options.dump_graphics = (number != 0);
    } else if (key == "audio_selftest") {
      options.audio_selftest = (number != 0);
    } else if (key == "blit_stats") {
      options.blit_stats = (number != 0);
    } else if (key == "export_jp_text") {
      options.export_jp_text = (number != 0);
    } else if (key == "time_budget_ms") {
      if (number > 0) options.time_budget_ms = number;
    } else if (key == "max_instructions") {
      if (number > 0) options.max_instructions = number;
    } else if (key == "frame_log_every") {
      if (number > 0) options.frame_log_every = number;
    } else if (key == "touch_button") {
      if (number > 0) options.touch_button = number;
    } else if (key == "lb_minigame") {
      options.lb_minigame = (number != 0);
    } else if (key == "dirty_gate") {
      options.dirty_gate = (number != 0);
    } else if (key == "dirty_stats") {
      options.dirty_stats = (number != 0);
    } else if (key == "slice_ms") {
      if (number >= 1 && number <= 40) options.slice_ms = number;
    } else if (key == "dump_frames") {
      if (number >= 1 && number <= 200) options.dump_frames = number;
    } else if (key == "exec_ignore_force_wait") {
      options.exec_ignore_force_wait = (number != 0);
    } else if (key == "dump_scenario") {
      options.dump_scenarios.clear();
      options.dump_all_scenarios = false;
      if (value == "all") {
        options.dump_all_scenarios = true;
        continue;
      }
      std::stringstream items(value);
      std::string item;
      while (std::getline(items, item, ',')) {
        if (!item.empty()) options.dump_scenarios.push_back(std::atoi(item.c_str()));
      }
    } else if (key == "loop_probe") {
      options.loop_probe = (number != 0);
    }
  }
  return options;
}

/**
 * 画面「内容」的稀疏哈希（诊断用，见 D-023）。
 *
 * 每次合成后与上一帧比一次，就能得到**内容真正变化的速率**——用户感知到的帧率是它，
 * 而不是主循环轮次、也不是合成次数。三者一旦差得多，就能立刻判断到底是
 * "游戏本来就没在动"还是"我们把它丢了"。
 */
uint64_t SparseFrameHash(AndroidGraphicsSystem& graphics) {
  std::shared_ptr<AndroidSurface> frame = graphics.frame_buffer();
  if (!frame) return 0;
  const Size size = frame->GetSize();
  const uint32_t* pixels = frame->pixels();
  if (pixels == nullptr || size.width() <= 0 || size.height() <= 0) return 0;
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  // 每 4 行取一行、每行每 4 像素取一个（覆盖 1/16 像素）。早先用 1/128 的稀疏采样，
  // 像「角色小幅动作」「眨眼」这类只动一小块画面的动画会被漏检，读数因此偏小。
  // 480000/16 = 30000 次哈希，代价仍然可以忽略。
  for (int y = 0; y < size.height(); y += 4) {
    const uint32_t* row = pixels + static_cast<size_t>(y) * size.width();
    for (int x = 0; x < size.width(); x += 4) {
      hash = (hash ^ row[x]) * 1099511628211ull;
    }
  }
  return hash;
}

/** 把图形系统当前的帧缓冲拷进呈现缓冲。 */
void CaptureFrame(AndroidGraphicsSystem& graphics) {
  std::shared_ptr<AndroidSurface> frame = graphics.frame_buffer();
  if (!frame) return;

  const Size size = frame->GetSize();
  const size_t count = static_cast<size_t>(size.width()) *
                       static_cast<size_t>(size.height());
  if (count == 0) return;

  std::lock_guard<std::mutex> lock(g_frame_mutex);
  g_frame_pixels.assign(frame->pixels(), frame->pixels() + count);
  g_frame_width = size.width();
  g_frame_height = size.height();
  ++g_frame_serial;

  // 非黑像素数：用于区分「引擎产出的帧本身是空的」与「呈现环节没显示出来」。
  // 帧日志可以按间隔抽样：逐帧输出在长时间运行时会淹没真正重要的诊断信息。
  if (g_frame_log_every <= 1 ||
      (g_frame_serial % static_cast<unsigned int>(g_frame_log_every)) == 1u) {
    // 注意：非黑像素统计要扫全屏 48 万像素，只在真的要打印时才做——
    // 之前它每帧都跑，是这个渲染管线里最没意义的一笔开销。
    size_t non_black = 0;
    for (size_t i = 0; i < count; ++i) {
      if ((g_frame_pixels[i] & 0x00FFFFFFu) != 0) ++non_black;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "frame %dx%d serial=%u nonblack=%zu/%zu", g_frame_width,
                        g_frame_height, g_frame_serial, non_black, count);
  }
}

/** 把 Java 字符串转成 UTF-8 的 std::string。 */
/**
 * 把当前呈现缓冲写成 PPM（P6）文件，供离线核对动画形态。
 *
 * 只在"内容真的变了"的时候调用（见主循环），因此每一张文件都对应一次真实的
 * 画面更新。把连续几张放在一起看，就能区分"资源本身就是逐帧图片切换"和
 * "连续位移/渐变被我们渲染成阶跃"——这是靠日志数字无法分辨的。
 */
void DumpFramePpm(int index) {
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    dir = g_diag_dir;
  }
  if (dir.empty()) return;

  std::vector<uint32_t> pixels;
  int width = 0;
  int height = 0;
  {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    pixels = g_frame_pixels;
    width = g_frame_width;
    height = g_frame_height;
  }
  if (width <= 0 || height <= 0 || pixels.empty()) return;

  char relative[64];
  std::snprintf(relative, sizeof(relative), "/frame-%04d.ppm", index);
  std::ofstream out(dir + relative, std::ios::binary);
  if (!out) return;

  out << "P6\n" << width << " " << height << "\n255\n";
  std::vector<char> rgb(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < pixels.size(); ++i) {
    const uint32_t pixel = pixels[i];
    rgb[i * 3 + 0] = static_cast<char>(pixel & 0xFFu);
    rgb[i * 3 + 1] = static_cast<char>((pixel >> 8) & 0xFFu);
    rgb[i * 3 + 2] = static_cast<char>((pixel >> 16) & 0xFFu);
  }
  out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "dumped frame %d (%dx%d) -> frame-%04d.ppm", index, width,
                      height, index);
}

std::string JStringToUtf8(JNIEnv* env, jstring value) {
  if (value == nullptr) return std::string();
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result = (chars != nullptr) ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
  return result;
}

/**
 * native -> Java 的字符串出口。**所有**返回给 Kotlin 的字符串都必须走这里。
 *
 * JNI 的 NewStringUTF 要求 Modified UTF-8；而游戏数据里的字符串往往是 Shift-JIS
 *（例如 Kud Wafter 的 #REGNAME = "KEY\クドわふたー"，首字节 0x83）。
 * 直接把这种字节交给 JNI 会触发
 *   "JNI DETECTED ERROR IN APPLICATION: input is not valid Modified UTF-8"
 * 并让 ART 直接 abort 整个进程——实测表现就是「跑一下应用就被杀」。
 */
jstring NewSafeJavaString(JNIEnv* env, const std::string& text) {
  if (utf8::is_valid(text.begin(), text.end())) {
    return env->NewStringUTF(text.c_str());
  }

  // 不是合法 UTF-8：按 CP932（RealLive 的原生编码）转一次再试。
  const std::string converted = cp932toUTF8(text, 0);
  if (utf8::is_valid(converted.begin(), converted.end())) {
    return env->NewStringUTF(converted.c_str());
  }

  // 兜底：替换掉非 ASCII 字节，保证一定能构造出 Java 字符串而不是让进程 abort。
  std::string sanitized;
  sanitized.reserve(text.size());
  for (unsigned char c : text) {
    sanitized.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return env->NewStringUTF(sanitized.c_str());
}

/**
 * 逐行输出报告。
 *
 * 必须逐行：logcat 对单条消息有长度上限（约 1000 字符），把几千字的报告塞进一次
 * __android_log_print 会让后半部分静默消失——实测中这会被误判成「运行卡住了」。
 */
void LogReport(const char* tag, const char* title, const std::string& report) {
  __android_log_print(ANDROID_LOG_INFO, tag, "%s:", title);
  std::string::size_type begin = 0;
  while (begin <= report.size()) {
    std::string::size_type end = report.find('\n', begin);
    if (end == std::string::npos) end = report.size();
    if (end > begin) {
      __android_log_print(ANDROID_LOG_INFO, tag, "%.*s", static_cast<int>(end - begin),
                          report.c_str() + begin);
    }
    begin = end + 1;
  }
}

/**
 * 供日志与报告显示用：把可能是 Shift-JIS 的游戏字符串转成 UTF-8。
 * 只在字符串不是合法 UTF-8 时才转换，因此 ASCII 与我们自己的文本原样保留。
 */
std::string ToDisplayUtf8(const std::string& text) {
  if (utf8::is_valid(text.begin(), text.end())) return text;
  const std::string converted = cp932toUTF8(text, 0);
  if (utf8::is_valid(converted.begin(), converted.end())) return converted;
  return std::string();
}

/** 返回版本串，用于验证 Kotlin <-> JNI 链路（T1.4 的最小验证）。 */
jstring VersionString(JNIEnv* env, jobject /*thiz*/) {
  return env->NewStringUTF("rlvm-android native bridge 0.1.0 (T0.2 skeleton)");
}

/** 返回指针位宽，用于确认实际加载的 ABI 与预期一致。 */
jint ProbeAbi(JNIEnv* /*env*/, jobject /*thiz*/) {
#if defined(__aarch64__)
  return 64;
#elif defined(__arm__)
  return 32;
#else
  return 0;
#endif
}

/**
 * 真正的引擎探针：在给定的目录里解析 Gameexe.ini 并打开 SEEN 归档。
 *
 * 这是 Android 上第一次执行 RLVM 的实际逻辑——会走通 boost::filesystem 访问、
 * libreallive 的 Gameexe 解析、以及 Archive + Mapping 的场景归档读取。
 * 返回值是供人阅读的多行报告；异常被捕获后也会写进报告，避免 native 崩溃。
 */
jstring ProbeGameDir(JNIEnv* env, jobject /*thiz*/, jstring jdir) {
  namespace fs = boost::filesystem;
  std::string report;

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    report += "dir = " + dir + "\n";

    const fs::path root(dir);
    if (!fs::exists(root)) {
      report += "ERROR: directory does not exist\n";
      return NewSafeJavaString(env, report);
    }
    if (!fs::is_directory(root)) {
      report += "ERROR: not a directory\n";
      return NewSafeJavaString(env, report);
    }

    report += "files:\n";
    for (fs::directory_iterator it(root); it != fs::directory_iterator(); ++it) {
      const fs::path& p = it->path();
      // 目录条目上调用 file_size 在 Android 上会抛「Function not implemented」，
      // 因此只对普通文件取大小。
      if (fs::is_directory(p)) {
        report += "  " + p.filename().string() + "/  <dir>\n";
      } else {
        report += "  " + p.filename().string() + "  " +
                  std::to_string(fs::file_size(p)) + " bytes\n";
      }
    }

    // --- Gameexe.ini ---
    // 上游的 CorrectPathCase 负责大小写纠正：RealLive 游戏来自 Windows/FAT，
    // 文件名大小写常不规整，而 Android 文件系统区分大小写。
    const fs::path gameexe_path = CorrectPathCase(root / "Gameexe.ini");
    if (!gameexe_path.empty()) {
      // 注意：Gameexe 位于全局命名空间，只有 Archive / Scenario 在 libreallive 里。
      Gameexe gameexe(gameexe_path);
      const std::string caption = gameexe("CAPTION").ToString("");
      report += "Gameexe: parsed OK";
      if (!caption.empty()) report += " (CAPTION=" + caption + ")";
      report += "\n";
    } else {
      report += "Gameexe: Gameexe.ini not found (even after case correction)\n";
    }

    // --- SEEN.TXT ---
    const fs::path seen_path = CorrectPathCase(root / "Seen.txt");
    if (!seen_path.empty()) {
      libreallive::Archive archive(seen_path.string());
      // ReadTOC() 把 (offset, length) 表映射成场景索引；offset 为 0 的条目不存在，
      // 所以索引不一定从 0 开始。这里直接遍历 TOC 拿到真实索引。
      int scenario_count = 0;
      std::string indices;
      for (libreallive::Archive::const_iterator it = archive.begin();
           it != archive.end(); ++it) {
        ++scenario_count;
        if (scenario_count <= 8) {
          if (!indices.empty()) indices += ",";
          indices += std::to_string(it->first);
        }
      }
      report += "Seen: TOC entries=" + std::to_string(scenario_count) +
                "; indices=[" + indices + "]";

      // 构造第一个场景会真正走通解压路径（compression.cc）。
      libreallive::Scenario* scenario =
          (archive.begin() != archive.end())
              ? archive.GetScenario(archive.begin()->first)
              : nullptr;
      report += std::string("; firstScenario=") +
                (scenario != nullptr ? "constructed" : "null");
      report += "; probableEncoding=" +
                std::to_string(archive.GetProbableEncodingType());
      report += "\n";
    } else {
      report += "Seen: Seen.txt not found (even after case correction)\n";
    }

    report += "PROBE OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "probe report", report);
  return NewSafeJavaString(env, report);
}

/**
 * 在真机上真正跑起引擎：装配 AndroidSystem + RLMachine + 全部指令模块，
 * 然后执行字节码，直到停机、进入长操作或耗尽指令预算。
 *
 * 这是 T2.4（引擎生命周期）的第一步——只跑不控。启动/暂停/恢复/退出
 * 的完整生命周期要等 AndroidSystem 接入渲染与事件源之后再补。
 */
/**
 * 装配 AndroidSystem + RLMachine 并执行字节码。
 * 「普通路径」与「SAF」两个入口共用这段逻辑，差异只在 Archive 怎么打开。
 */
/**
 * 汉化支持（见 docs/LOCALIZATION.md）：把每个场景的文本串按顺序导出成 TSV。
 *
 * 导出顺序 = 「场景号升序、场景内出现顺序」。这个顺序必须可靠，因为它是后续
 * 与中文译文对齐的唯一依据；序章「心臓がドキドキ…」应能在结果里找到。
 */
void ExportSceneText(libreallive::Archive& archive,
                     const std::string& out_dir,
                     std::string& report) {
  if (out_dir.empty()) {
    report += "export_jp_text: no diagnostics dir; skipped\n";
    return;
  }

  // 诊断：常驻内存探针 + 自身上限。真机上出现过「解析到一半内存暴涨、被系统
  // lowmemorykiller SIGKILL，连带把别的应用一起杀」的情况——这里把内存曲线和
  // 每个场景的元素数打进日志，并在超过上限时主动停手，避免再把整机拖下水。
  LogMemory("export start");
  std::ostringstream out;
  int scenes = 0;
  int strings = 0;
  std::vector<int> failed;
  for (auto it = archive.begin(); it != archive.end(); ++it) {
    const int index = it->first;
    const long rss_before = CurrentRssKb();
    if (rss_before > kMemoryGuardKb) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "export_jp_text: aborted before scene %d, rss=%ld kB > %ld kB",
                          index, rss_before, kMemoryGuardKb);
      report += "export_jp_text: aborted at scene " + std::to_string(index) +
                " (rss guard)\n";
      break;
    }
    // 逐场景容错：解析失败（例如汉化补丁改写过的菜单场景，select 结构不是
    // RLVM 期望的布局）只跳过这一个场景并记账，否则一个坏场景会让整份导出
    // ——以及"到底哪些场景坏"这个诊断结论——全都拿不到。
    libreallive::Scenario* scenario = nullptr;
    try {
      scenario = archive.GetScenario(index);
    } catch (const std::exception& e) {
      failed.push_back(index);
      report += "export_jp_text: scene " + std::to_string(index) +
                " parse failed: " + e.what() + "\n";
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "export_jp_text: scene %d parse failed: %s", index,
                          e.what());
      continue;
    }
    if (scenario == nullptr) continue;
    ++scenes;

    // 该场景自己声明的文本编码：0 = CP932（默认），1 = CP936/GBK。
    // 语义与渲染路径完全一致（RLMachine::GetTextEncoding() 就是
    // call_stack_.back().scenario->encoding()），**不能**在 0 时回退到
    // Archive::GetProbableEncodingType()——那是整库默认值，用在逐场景上会把
    // 未覆盖的日文场景（声明 0）也按 GBK 解成乱码。
    const int encoding = scenario->encoding();

    // 元素数：正常场景是「几千」量级；被改坏的数据会让扫描器一路错位，把整段
    // 数据切成十万级甚至百万级元素——这就是内存暴涨的直接来源，先量出来。
    int elements = 0;
    for (auto element = scenario->begin(); element != scenario->end(); ++element) {
      ++elements;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "export scene %d: dlen=%zu elements=%d enc=%d rss=%ld kB",
                        index, it->second.length, elements, encoding, rss_before);

    int ordinal = 0;
    for (auto element = scenario->begin(); element != scenario->end(); ++element) {
      // BytecodeList 是 forward_list<unique_ptr<BytecodeElement>>，取 get() 判型。
      const libreallive::TextoutElement* text =
          dynamic_cast<const libreallive::TextoutElement*>((*element).get());
      if (text == nullptr) continue;
      // 按场景声明的编码转成 UTF-8，便于比对与显示。
      out << index << '\t' << ordinal++ << '\t'
          << cp932toUTF8(text->GetText(), encoding)
          << '\n';
      ++strings;
    }
  }

  const std::string path = out_dir + "/jp-text.tsv";
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    report += "export_jp_text: cannot write " + path + "\n";
    return;
  }
  const std::string payload = out.str();
  file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  file.close();
  report += "export_jp_text: scenes=" + std::to_string(scenes) +
            " strings=" + std::to_string(strings) +
            " failed=" + std::to_string(failed.size()) + " -> " + path + "\n";
}

/**
 * 诊断：把某个场景反汇编成 RLVM 的「源码形式」（指令名 + 参数），写进诊断目录。
 *
 * 系统菜单/存档读档这类界面是游戏自带脚本实现的，只有反汇编出来才能知道它到底
 * 调用了哪些指令——「存档能写、读档列表为空」这类问题用它定位比盲猜 opcode 可靠。
 */
void DumpScenarioToFile(libreallive::Archive& archive,
                        const std::string& out_dir,
                        int scene_number,
                        std::string& report) {
  if (out_dir.empty()) {
    report += "dump_scenario: no diagnostics dir; skipped\n";
    return;
  }

  std::ostringstream oss;
  libreallive::Scenario* scenario = nullptr;
  try {
    scenario = archive.GetScenario(scene_number);
  } catch (const std::exception& e) {
    oss << "// SEEN" << scene_number << " parse failed: " << e.what() << "\n";
  }
  if (scenario != nullptr) {
    for (auto const& instruction : *scenario)
      instruction->PrintSourceRepresentation(oss);
  }

  const std::string path =
      out_dir + "/scenario-" + std::to_string(scene_number) + ".txt";
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    report += "dump_scenario: cannot write " + path + "\n";
    return;
  }
  const std::string payload = oss.str();
  file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  file.close();
  report += "dump_scenario: SEEN" + std::to_string(scene_number) +
            (scenario != nullptr ? " ok" : " (no scenario)") + " -> " + path +
            " (" + std::to_string(payload.size()) + " bytes)\n";
}

/** 诊断：把整库场景反汇编到一个文件，前面带 `#scene N` 分隔标记。 */
void DumpAllScenariosToFile(libreallive::Archive& archive,
                            const std::string& out_dir,
                            std::string& report) {
  if (out_dir.empty()) {
    report += "dump_scenario(all): no diagnostics dir; skipped\n";
    return;
  }

  std::ostringstream oss;
  int dumped = 0;
  for (auto it = archive.begin(); it != archive.end(); ++it) {
    const int index = it->first;
    libreallive::Scenario* scenario = nullptr;
    try {
      scenario = archive.GetScenario(index);
    } catch (const std::exception& e) {
      oss << "#scene " << index << " PARSE FAILED: " << e.what() << "\n";
      continue;
    }
    if (scenario == nullptr) continue;
    oss << "#scene " << index << "\n";
    for (auto const& instruction : *scenario)
      instruction->PrintSourceRepresentation(oss);
    ++dumped;
  }

  const std::string path = out_dir + "/scenarios-all.txt";
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    report += "dump_scenario(all): cannot write " + path + "\n";
    return;
  }
  const std::string payload = oss.str();
  file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  file.close();
  report += "dump_scenario(all): scenes=" + std::to_string(dumped) + " -> " +
            path + " (" + std::to_string(payload.size()) + " bytes)\n";
}

void RunEngineOn(System& system,
                 Gameexe& gameexe,
                 libreallive::Archive& archive,
                 int max_instructions,
                 std::string& report) {
  // 分步日志：真机上「跑很久却没有任何输出」时，用它定位卡在哪一步。
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: constructing RLMachine");
  LogMemory("engine start");

  // 触摸输入（T2.3）：把当前系统暴露给 UI 线程，离开 RunEngineOn 时自动断开。
  CurrentSystemGuard current_system(dynamic_cast<AndroidSystem*>(&system));

  RLMachine machine(system, archive);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddAllModules");
  AddAllModules(machine);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddGameHacks");
  AddGameHacks(machine);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: machine ready");
  LogMemory("machine ready");
  // 与上游 RLVMInstance 一致：遇到指令异常时跳过该指令继续执行，
  // 而不是把整台机器标记为 halted。
  machine.SetHaltOnException(false);
  // 打印未实现的操作码与指令异常：上游默认把它们吞掉，
  // 在 Android 上 std::cerr 又看不到，于是表现为「跑了很多指令却什么都没发生」。
  machine.SetPrintUndefinedOpcodes(true);

  // 与上游 RLVMInstance 一致：启动时载入"全局数据"（Config：音量、文字速度、
  // 画面设置等都在里面）。之前我们绕过了 RLVMInstance，这两步就漏掉了——
  // 结果是配置写得下去却永远读不回来，表现为"config 不保留"。
  try {
    Serialization::loadGlobalMemory(machine);
    report += "global memory loaded\n";
  } catch (const std::exception& e) {
    // 首次运行没有 global.sav.gz，属于正常情况。
    report += std::string("global memory not loaded (first run?): ") + e.what() + "\n";
  }

  // 设备侧诊断参数（文件不存在时全部取缺省值，行为与之前一致）。
  const DiagOptions diag = LoadDiagOptions();
  g_frame_log_every = diag.frame_log_every;
  g_touch_buttons.store(diag.touch_button);
  SetBlitStatsEnabled(diag.blit_stats);
  // 合帧策略（D-022）：默认与上游一致（脏标记驱动）。
  SetDirtyGateEnabled(diag.dirty_gate);
  SetDirtyStatsEnabled(diag.dirty_stats);
  // 逆向 PT00 用：诊断文件里 lb_minigame=1 时不再跳过 LB/LBEX 的棒球小游戏，
  // 让脚本跑进去调用 PT00.dll（调用记录见 little_busters_pt00dll.cc）。默认仍跳过。
  SetLBSkipBaseball(!diag.lb_minigame);
  if (diag.max_instructions > 0) max_instructions = diag.max_instructions;
  if (diag.trace) machine.set_tracing_on();

  // REGNAME 是 CP932 编码的游戏数据，显示前先转成 UTF-8——
  // 否则报告本身含有非法 UTF-8 字节。
  report += "engine assembled (regname=\"" +
            ToDisplayUtf8(gameexe("REGNAME").ToString("")) + "\")\n";
  report += "diagnostics: trace=" + std::string(diag.trace ? "on" : "off") +
            " loop_probe=" + std::string(diag.loop_probe ? "on" : "off") +
            " blit_stats=" + std::string(diag.blit_stats ? "on" : "off") +
            " time_budget_ms=" + std::to_string(diag.time_budget_ms) +
            " max_instructions=" + std::to_string(max_instructions) +
            " frame_log_every=" + std::to_string(diag.frame_log_every) +
            " lb_minigame=" + std::string(diag.lb_minigame ? "on" : "off") +
            " dirty_gate=" + std::string(diag.dirty_gate ? "on" : "off") +
            " slice_ms=" + std::to_string(diag.slice_ms) + "\n";

  // 重采样自检：把「音调是否偏高」变成日志里的一个频率数字。
  if (diag.audio_selftest) report += rlvm_android::ResamplerSelfTest();

  // 汉化支持：导出各场景日文文本串（见 docs/LOCALIZATION.md）。
  if (diag.export_jp_text) {
    std::string export_dir;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      export_dir = g_diag_dir;
    }
    ExportSceneText(archive, export_dir, report);
  }

  // 诊断：反汇编指定场景（存档/读档/系统菜单等脚本）。
  if (!diag.dump_scenarios.empty() || diag.dump_all_scenarios) {
    std::string dump_dir;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      dump_dir = g_diag_dir;
    }
    for (const int scene_number : diag.dump_scenarios)
      DumpScenarioToFile(archive, dump_dir, scene_number, report);
    if (diag.dump_all_scenarios)
      DumpAllScenariosToFile(archive, dump_dir, report);
  }
  LogMemory("after export");

  AndroidGraphicsSystem* graphics =
      dynamic_cast<AndroidGraphicsSystem*>(&system.graphics());

  // 与上游 RLVMInstance::Run 相同的结构：每轮先让子系统跑一遍（含合成一帧），
  // 再以 10ms 为时间片连续执行字节码。
  const unsigned int time_budget_ms = static_cast<unsigned int>(diag.time_budget_ms);
  const int slice_ms = diag.slice_ms;
  const unsigned int started = system.event().GetTicks();
  // 只统计本次运行造成的合成量，先清零。
  TakeGraphicsBlitStats();
  g_stop_requested.store(false);
  int executed = 0;
  int frames_presented = 0;
  // 已合帧计数（AndroidGraphicsSystem::frame_count_）：用来判断这一轮是否真的
  // 合过一帧（D-022 之后合帧是脏标记驱动的）。
  unsigned int last_composed = 0;
  // 主循环周期统计（节奏抖动是"卡顿感"的直接来源，见 D-023）。
  unsigned int last_loop_start = 0;
  unsigned int dt_min = 0xFFFFFFFFu, dt_max = 0, dt_sum = 0, dt_count = 0;
  // 内容变化计数：每次合成后与上一帧比一次哈希（见 SparseFrameHash 的说明）。
  uint64_t last_frame_hash = 0;
  unsigned int content_changes = 0;
  // 内容变化「间隔」直方图（毫秒）：用户感知到的帧率是内容变化的间隔，而不是
  // 循环轮次。直方图能区分「稳定 60fps」与「稳定 10fps 但中间夹着重复帧」——
  // 后者看起来就是逐帧动画式的顿（见 D-023）。
  unsigned int last_change_ticks = 0;
  unsigned int change_hist[6] = {0, 0, 0, 0, 0, 0};
  // 主循环三阶段耗时（毫秒累计）：把「每轮十几毫秒」拆成
  // 子系统/合帧（run）、字节码执行（exec）、让出 CPU（wait）三份。
  unsigned int phase_run_ms = 0;
  unsigned int phase_exec_ms = 0;
  unsigned int phase_wait_ms = 0;
  // 「呈现交接」耗时（SparseFrameHash + CaptureFrame）。这段原本不落在
  // run/exec/wait 任何一个相位里，是"每帧成本"的黑洞；图层叠加类动画每帧内容
  // 都在变，所以这段的成本直接决定它们的观感。
  uint64_t phase_present_us = 0;
  // 窗口内「最慢的单条指令」：执行阶段每轮十几毫秒，必须定位到具体是哪条
  //（连同场景号和行号），否则只能盲猜。见 D-023。
  uint64_t slow_op_us = 0;
  int slow_op_scene = -1;
  int slow_op_line = -1;
  unsigned int report_executed = 0;
  // 上一次打印统计时的状态：用来算窗口内的真实速率（窗口不一定正好 1 秒）。
  unsigned int report_composed = 0;
  unsigned int report_ticks = started;
  int dumped_frames = 0;
  int tree_dumps = 0;
  // 长操作推进计数：合帧 54 次/秒但内容只变 8 次/秒，说明"每次合帧都拿到同一个
  // 画面"。长操作（Effect/过场）是唯一能产生新画面的东西，必须确认它是否每轮推进。
  unsigned int long_op_rounds = 0;
  // intC[1] 的上次打印值：只在变化时输出（见循环内的诊断）。
  int last_logged_c1 = -1;
  // 主循环解剖（loop_probe）：把「每轮主循环到底把时间花在哪」拆开计数。
  // 关键是把**字节码指令**与**long op 步进**分开——两者都算 executed，
  // 混在一起就永远看不出"每轮只有 1 条指令"是被 long op 卡住还是别的原因。
  unsigned int probe_rounds = 0;
  unsigned int probe_bytecode_ops = 0;
  unsigned int probe_longop_steps = 0;
  unsigned int probe_longop_rounds = 0;      // 退出 exec 片时栈顶是 long op 的轮数
  unsigned int probe_force_wait_rounds = 0;  // 退出时 force_wait 仍为真的轮数
  std::map<std::string, unsigned int> probe_longop_types;
  std::map<int, unsigned int> probe_scene_ops;
  // 场景号:行号 -> 执行次数（key = scene<<32 | line）。用来判断"最热场景里
  // 那几百万条指令到底花在哪一行"——是循环体在空转，还是别处。
  std::map<uint64_t, unsigned int> probe_line_ops;
  unsigned int probe_report_ticks = started;
  std::string stop_reason = "instruction budget exhausted";

  while (executed < max_instructions) {
    {
      const unsigned int loop_now = system.event().GetTicks();
      if (last_loop_start != 0) {
        const unsigned int dt = loop_now - last_loop_start;
        if (dt < dt_min) dt_min = dt;
        if (dt > dt_max) dt_max = dt;
        dt_sum += dt;
        ++dt_count;
      }
      last_loop_start = loop_now;
    }
    if (g_stop_requested.load()) {
      stop_reason = "stop requested";
      break;
    }
    // 挂起（黑屏/应用进后台，见 SetEngineSuspended）：完全不推进——不跑 system.Run
    // （不合成新帧）、不执行字节码，只让出 CPU 等唤醒。音频侧同时静音并暂停回调。
    if (g_engine_suspended.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (machine.halted()) {
      stop_reason = "machine halted";
      break;
    }
    // time_budget_ms == 0 表示不限时：应用要能一直停在标题/正文上。
    if (time_budget_ms > 0 &&
        system.event().GetTicks() - started > time_budget_ms) {
      stop_reason = "time budget exhausted";
      break;
    }

    const unsigned int run_begin_ticks = system.event().GetTicks();
    system.Run(machine);
    phase_run_ms += system.event().GetTicks() - run_begin_ticks;
    if (graphics != nullptr) {
      // 只在真正合过帧时才拷进呈现缓冲（D-022：合帧已改为脏标记驱动）。
      // 静止画面不再每轮重复拷贝/上传；GL 线程本来就只在帧序号变化时重传。
      const auto present_start = std::chrono::steady_clock::now();
      const unsigned int composed = graphics->frame_count();
      if (composed != last_composed) {
        last_composed = composed;
        const uint64_t hash = SparseFrameHash(*graphics);
        if (hash != last_frame_hash) {
          last_frame_hash = hash;
          ++content_changes;
          const unsigned int change_ticks = system.event().GetTicks();
          if (last_change_ticks != 0) {
            const unsigned int gap = change_ticks - last_change_ticks;
            if (gap < 8) ++change_hist[0];
            else if (gap < 16) ++change_hist[1];
            else if (gap < 33) ++change_hist[2];
            else if (gap < 66) ++change_hist[3];
            else if (gap < 133) ++change_hist[4];
            else ++change_hist[5];
          }
          last_change_ticks = change_ticks;
          // 只有内容真的变了才发布新帧：不然 GL 线程每轮都要重传 1.9MB 纹理
          //（动画是低频内容时，这是纯浪费——见 D-023）。
          CaptureFrame(*graphics);
          if (diag.dump_frames > 0 && dumped_frames < diag.dump_frames) {
            ++dumped_frames;
            DumpFramePpm(dumped_frames);
          }
          // 内容变化瞬间的图形栈（含每个对象的 alpha 与活跃 mutator）。
          // "alpha 为什么每 117ms 跳一个大台阶"只有这一刻的现场能回答。
          if (diag.dump_graphics && tree_dumps < 20) {
            ++tree_dumps;
            std::ostringstream tree;
            graphics->Refresh(&tree);
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "tree| ---- change #%d ----", tree_dumps);
            std::istringstream lines(tree.str());
            std::string line;
            while (std::getline(lines, line)) {
              if (!line.empty()) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag, "tree| %s",
                                    line.c_str());
              }
            }
          }
        }
      }
      phase_present_us += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - present_start)
              .count());
      ++frames_presented;
    }

    // intC[1] 每次变化都打一行（诊断）：这是脚本喂给 index_series 的"已过时间"。
    // 它变化的粒度 = 淡变台阶的粗细。只在变化时输出，日志量很小。
    if (diag.dirty_stats) {
      const int c1 = machine.GetIntValue(
          libreallive::IntMemRef(libreallive::INTC_LOCATION, 0, 1));
      if (c1 != last_logged_c1) {
        last_logged_c1 = c1;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "intC1 -> %d (intL0=%d, t=%ums)", c1,
                            machine.GetIntValue(libreallive::IntMemRef(
                                libreallive::INTL_LOCATION, 0, 0)),
                            system.event().GetTicks() - started);
      }
    }

    // 进度日志：定位「跑很久但没有输出」这类问题。
    if (frames_presented % 60 == 0) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "progress: loops=%d composed=%u instructions=%d elapsed=%ums rss=%ldkB",
                          frames_presented,
                          graphics != nullptr ? graphics->frame_count() : 0u,
                          executed,
                          system.event().GetTicks() - started, CurrentRssKb());
      if (diag.dirty_stats) {
        // 本统计窗口的轮数（dt_count 在下面 loop dt 打印后会被清零，先留一份）。
        unsigned int window_loops = 0;
        unsigned int window_dt_sum = 0;
        // 谁在标脏（dc0/hik/obj/text/mouse）——用来判断合帧率低的来源。
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "dirty: %s",
                            TakeDirtyStatsSummary().c_str());
        // 合成成本：这一秒里 blit 了多少次、写了多少像素、花了多少时间。
        // 用来回答"每帧十几毫秒到底花在哪"——像素数是关键（480000 = 一屏）。
        uint64_t blit_calls = 0, blit_pixels = 0, blit_us = 0;
        TakeBlitCostSummary(blit_calls, blit_pixels, blit_us);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "blit cost: calls=%llu pixels=%llu time=%.1fms/screens=%.2f",
                            static_cast<unsigned long long>(blit_calls),
                            static_cast<unsigned long long>(blit_pixels),
                            blit_us / 1000.0,
                            blit_pixels / 480000.0);
        // 合成路径：fast 应该是绝大多数（背景/立绘整行 memcpy）。若 slow 占多数，
        // 说明「整面不透明」判定没建立起来，逐像素混合就是这十几毫秒的来源。
        {
          uint64_t fast_calls = 0, slow_calls = 0;
          TakeBlitPathSummary(fast_calls, slow_calls);
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "blit path: fast=%llu slow=%llu calls",
                              static_cast<unsigned long long>(fast_calls),
                              static_cast<unsigned long long>(slow_calls));
        }
        uint64_t worst_us = 0;
        const std::string worst = TakeBlitWorstSummary(worst_us);
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "blit worst: %.1fms  %s", worst_us / 1000.0,
                            worst.c_str());
        // 主循环节奏：min/avg/max 三件套。avg 与 max 差得远 = 抖动（动画会顿）。
        if (dt_count > 0) {
          window_loops = dt_count;
          window_dt_sum = dt_sum;
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "loop dt: min=%ums avg=%.1fms max=%ums (n=%u)",
                              dt_min, static_cast<double>(dt_sum) / dt_count, dt_max,
                              dt_count);
          dt_min = 0xFFFFFFFFu;
          dt_max = 0;
          dt_sum = 0;
          dt_count = 0;
        }
        // 内容变化率：这才是用户看到的"帧率"。窗口不一定是整 1 秒，
        // 因此按实际窗口长度归一化（早先直接把窗口内计数当成 /s，会偏读）。
        const unsigned int window_ticks = system.event().GetTicks() - report_ticks;
        const unsigned int window_ms = window_ticks > 0 ? window_ticks : 1;
        const unsigned int composed_now =
            graphics != nullptr ? graphics->frame_count() : 0u;
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "content: changed=%.1f/s (composites=%.1f/s window=%ums)",
                            content_changes * 1000.0 / window_ms,
                            (composed_now - report_composed) * 1000.0 / window_ms,
                            window_ms);
        // 内容变化间隔分布：稳定 10fps 的样子是 33-66/>133 占满；
        // 稳定 60fps 的样子是 8-16/16-33 占满。两者观感完全不同。
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                            "content dt: <8ms=%u 8-16=%u 16-33=%u 33-66=%u 66-133=%u >133=%u",
                            change_hist[0], change_hist[1], change_hist[2],
                            change_hist[3], change_hist[4], change_hist[5]);
        // 每轮耗时构成（ms 累计 ÷ 轮数，给出三阶段的平均毫秒）。
        {
          // 注意除数必须是**本窗口的轮数**：早先用累计 frames_presented，
          // 会让读数随运行时间越来越小（刚上机时每轮 16ms 被显示成 0.23ms）。
          const double loops = window_loops > 0 ? window_loops : 1.0;
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "phase cost: run=%.2fms exec=%.2fms wait=%.2fms "
                              "present=%.2fms other=%.2fms per-loop",
                              phase_run_ms / loops, phase_exec_ms / loops,
                              phase_wait_ms / loops, phase_present_us / 1000.0 / loops,
                              (static_cast<double>(window_dt_sum) -
                               (phase_run_ms + phase_exec_ms + phase_wait_ms)) /
                                  loops);
        }
        // 执行阶段细节：本窗口执行了多少条指令、最慢的一条在哪里。
        // 「每轮 14ms 却只跑 2 条指令」必须看到指令本体才能继续。
        {
          const unsigned int ops = executed - report_executed;
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "exec detail: ops=%u slowest=%.1fms scene=%d line=%d",
                              ops, slow_op_us / 1000.0, slow_op_scene,
                              slow_op_line);
          // 关键判据：本窗口有多少轮"结尾时长操作仍挂着"。
          // 若 ≈ 轮数，说明长操作每轮都在推进（那画面理应每轮都变）；
          // 若远小于轮数，说明多数轮次根本没跑到长操作，画面自然不变。
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "long op: rounds=%u of %u loops", long_op_rounds,
                              window_loops);
          // 动画计时变量现场：脚本用 intC[0]=剩余时间 / intC[1]=已过时间 驱动
          // index_series，intL[0] 是喂给 objChildAlpha 的值。直接读出来看取值
          // 范围与更新粒度（不依赖任何写入探针是否命中）。
          __android_log_print(
              ANDROID_LOG_INFO, kLogTag, "vars: intC[0]=%d intC[1]=%d intL[0]=%d",
              machine.GetIntValue(libreallive::IntMemRef(
                  libreallive::INTC_LOCATION, 0, 0)),
              machine.GetIntValue(libreallive::IntMemRef(
                  libreallive::INTC_LOCATION, 0, 1)),
              machine.GetIntValue(libreallive::IntMemRef(
                  libreallive::INTL_LOCATION, 0, 0)));
        }
        content_changes = 0;
        change_hist[0] = change_hist[1] = change_hist[2] = 0;
        change_hist[3] = change_hist[4] = change_hist[5] = 0;
        phase_run_ms = phase_exec_ms = phase_wait_ms = 0;
        phase_present_us = 0;
        slow_op_us = 0;
        slow_op_scene = -1;
        slow_op_line = -1;
        report_executed = executed;
        long_op_rounds = 0;
        report_composed = composed_now;
        report_ticks = system.event().GetTicks();
      }
      // 自我保护：真机上出现过内存暴涨把整机拖垮（系统连带杀掉别的应用）。
      // 宁可让引擎自己停下来，也不要让系统去杀。
      if (CurrentRssKb() > kMemoryGuardKb) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "memory guard: rss=%ld kB > %ld kB, stopping engine",
                            CurrentRssKb(), kMemoryGuardKb);
        stop_reason = "memory guard";
        break;
      }
    }

    // 音频在运行期的周期性证据：active_channels>0 表示游戏自己点的 BGM 还在播；
    // peak 取「本窗口内」的最大值，所以每报一次就清零，避免看成一个累计数。
    if (frames_presented % 300 == 0) {
      rlvm_android::AudioEngine& audio = rlvm_android::AudioEngine::Instance();
      const rlvm_android::AudioEngine::Stats audio_stats = audio.GetStats();
      __android_log_print(ANDROID_LOG_INFO, "rlvm-audio",
                          "runtime: active_channels=%d peak_in_window=%d frames=%llu",
                          audio_stats.active_channels, audio_stats.peak_amplitude,
                          audio_stats.frames_rendered);
      audio.ResetPeak();
    }


    // 上游在遇到长操作时只跳出**内层**时间片（把控制权让给这一帧），
    // 外层循环继续推进——长操作本身由后续的 ExecuteNextInstruction 轮询。
    // 早先这里直接 break 整个循环，导致游戏停在第一个长操作上不再前进。
    const unsigned int slice_start = system.event().GetTicks();
    unsigned int now = slice_start;
    do {
      const auto instruction_start = std::chrono::steady_clock::now();
      // 分类：栈顶是 long op 时，ExecuteNextInstruction 只是推进它，不执行任何字节码。
      // 两者必须分开计数，否则"每轮只有 1 条指令"无法归因。
      const std::shared_ptr<LongOperation> top_op = machine.CurrentLongOperation();
      if (diag.loop_probe) {
        if (top_op) {
          ++probe_longop_steps;
          ++probe_longop_types[typeid(*top_op).name()];
        } else {
          ++probe_bytecode_ops;
          const int scene = machine.SceneNumber();
          ++probe_scene_ops[scene];
          const uint64_t key =
              (static_cast<uint64_t>(static_cast<uint32_t>(scene)) << 32) |
              static_cast<uint32_t>(machine.line_number());
          ++probe_line_ops[key];
        }
      }
      machine.ExecuteNextInstruction();
      const uint64_t instruction_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - instruction_start)
              .count());
      if (instruction_us > slow_op_us) {
        slow_op_us = instruction_us;
        slow_op_scene = machine.SceneNumber();
        slow_op_line = machine.line_number();
      }
      ++executed;
      now = system.event().GetTicks();
    } while (!machine.CurrentLongOperation() &&
             (!system.force_wait() || diag.exec_ignore_force_wait) &&
             (now - slice_start < slice_ms));
    // 说明见 DiagOptions::exec_ignore_force_wait：上游 SDL 后端用 force_wait 在
    // refresh() 时让出时间片给视频刷新；我们的合帧由主循环每轮统一完成，
    // 因此它唯一的实际效果是把"一圈脚本"切成十几轮才转完（实测 128ms/圈，
    // 让 10000 级的淡变只剩 8 级台阶）。默认忽略它。
    if (diag.loop_probe) {
      ++probe_rounds;
      if (machine.CurrentLongOperation()) ++probe_longop_rounds;
      if (system.force_wait()) ++probe_force_wait_rounds;
    }
    system.set_force_wait(false);
    phase_exec_ms += system.event().GetTicks() - slice_start;
    if (machine.CurrentLongOperation() != nullptr) ++long_op_rounds;

    // 与上游 RLVMInstance::Run 一致：把这一轮补满到 slice_ms，再进入下一轮。
    //
    //   if (!sdlSystem.ShouldFastForward()) {
    //     int real_sleep_time = 10 - (end_ticks - start_ticks);
    //     if (real_sleep_time < 1) real_sleep_time = 1;
    //     sdlSystem.event().Wait(real_sleep_time);
    //   }
    //
    // 上游的 10ms 对应 ~100 轮/秒；手机屏幕普遍是 120Hz，100 与 120 不成整数比，
    // 显示端会交替显示 1 / 2 个 vsync，观感就是抖（D-023）。因此这里的时间片默认
    // 压到 6ms（≈150 轮/秒），让每个 vsync 都能取到新帧；诊断文件 slice_ms=N 可调。
    if (!system.ShouldFastForward()) {
      const unsigned int slice_elapsed = system.event().GetTicks() - slice_start;
      const int real_sleep_time = slice_elapsed >= static_cast<unsigned int>(slice_ms)
                                      ? 1
                                      : slice_ms - static_cast<int>(slice_elapsed);
      const unsigned int wait_begin_ticks = system.event().GetTicks();
      system.event().Wait(static_cast<unsigned int>(real_sleep_time));
      phase_wait_ms += system.event().GetTicks() - wait_begin_ticks;
    }

    // ---- 主循环解剖：每秒一行（loop_probe=1 时） ----
    // 一次性回答："每轮主循环的 1 条指令" 到底是 long op 卡的，还是别的。
    if (diag.loop_probe) {
      const unsigned int probe_now = system.event().GetTicks();
      if (probe_now - probe_report_ticks >= 1000) {
        const unsigned int window = probe_now - probe_report_ticks;
        probe_report_ticks = probe_now;
        const double per_s = 1000.0 / window;
        __android_log_print(
            ANDROID_LOG_INFO, kLogTag,
            "loop probe: %ums rounds=%u(%.0f/s) bytecode=%u(%.0f/s) "
            "longop_steps=%u(%.0f/s) longop_rounds=%u fw_rounds=%u",
            window, probe_rounds, probe_rounds * per_s, probe_bytecode_ops,
            probe_bytecode_ops * per_s, probe_longop_steps,
            probe_longop_steps * per_s, probe_longop_rounds,
            probe_force_wait_rounds);
        for (const auto& kv : probe_longop_types) {
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "loop probe:   longop %s -> %u steps",
                              kv.first.c_str(), kv.second);
        }
        // 字节码都花在哪个场景上（最热 3 个）。
        for (int rank = 0; rank < 3; ++rank) {
          int best_scene = -1;
          unsigned int best = 0;
          for (const auto& kv : probe_scene_ops) {
            if (kv.second > best) {
              best = kv.second;
              best_scene = kv.first;
            }
          }
          if (best_scene < 0) break;
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "loop probe:   scene %d -> %u ops", best_scene,
                              best);
          probe_scene_ops.erase(best_scene);
        }
        // 最热的 5 行：直接看几百万条指令落在脚本的哪一行上。
        for (int rank = 0; rank < 8; ++rank) {
          uint64_t best_key = 0;
          unsigned int best = 0;
          for (const auto& kv : probe_line_ops) {
            if (kv.second > best) {
              best = kv.second;
              best_key = kv.first;
            }
          }
          if (best == 0) break;
          const int scene = static_cast<int>(best_key >> 32);
          const int line = static_cast<int>(
              static_cast<uint32_t>(best_key & 0xFFFFFFFFull));
          __android_log_print(ANDROID_LOG_INFO, kLogTag,
                              "loop probe:   SEEN%d:%d -> %u ops", scene, line,
                              best);
          probe_line_ops.erase(best_key);
        }
        probe_scene_ops.clear();
        probe_line_ops.clear();
        probe_rounds = probe_bytecode_ops = probe_longop_steps = 0;
        probe_longop_rounds = probe_force_wait_rounds = 0;
        probe_longop_types.clear();
      }
    }
  }

  report += "instructions executed = " + std::to_string(executed) + "\n";
  report += "frames presented = " + std::to_string(frames_presented) + "\n";
  report += "stop reason = " + stop_reason + "\n";
  report += "halted = " + std::string(machine.halted() ? "yes" : "no") + "\n";
  const GraphicsBlitStats blits = TakeGraphicsBlitStats();
  if (diag.blit_stats) {
    report += "graphics blits: calls=" + std::to_string(blits.calls) +
              " written_pixels=" + std::to_string(blits.written_pixels) +
              " nonblack_pixels=" + std::to_string(blits.nonblack_pixels) + "\n";
  }

  // 图形栈转储：上游的 GraphicsSystem::Refresh(ostream*) 会把每个对象渲染时的
  // src/dst 矩形、alpha、可见性一并打印出来。这是判断「对象没被画」与
  // 「对象画到了屏幕外/全透明」最直接的手段。
  if (diag.dump_graphics && graphics != nullptr) {
    std::ostringstream tree;
    graphics->Refresh(&tree);
    report += "graphics tree dump:\n" + tree.str();
  }

  // 注意：这里**不再**主动播 BGM01。那段脚手架验证代码会在游戏自己
  // 请求标题曲之后抢走通道，再 BgmStop() 把游戏音乐停掉——真机上表现就是
  //「开头能听到一点，随后被测试音乐打断，然后彻底没声音」。
  // 现在音频完全由游戏脚本驱动（日志里可见 play channel=30 file=BGM/BGM14.nwa）。
  // 音频链路的证据改为运行期周期上报：见下方 progress 日志里的 audio 行。

  // 与上游 RLVMInstance 的收尾一致：把全局数据写回（Config 等）。
  // 写在 RunEngineOn 退出前，也就是用户点「停止引擎」或时间片用尽时。
  try {
    Serialization::saveGlobalMemory(machine);
    report += "global memory saved\n";
  } catch (const std::exception& e) {
    report += std::string("global memory save failed: ") + e.what() + "\n";
  }
}

jstring RunScenario(JNIEnv* env, jobject /*thiz*/, jstring jdir,
                    jint max_instructions) {
  namespace fs = boost::filesystem;
  std::string report;

  // 默认不限时运行，重复点击会起第二台引擎；这里拒绝并存。
  RunGuard guard;
  if (!guard.acquired()) {
    report += "ERROR: 已有一台引擎在运行，请先点「停止引擎」。\n";
    LogReport(kLogTag, "run report", report);
    return NewSafeJavaString(env, report);
  }

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    const fs::path root(dir);
    const fs::path gameexe_path = CorrectPathCase(root / "Gameexe.ini");
    const fs::path seen_path = CorrectPathCase(root / "Seen.txt");

    if (gameexe_path.empty() || seen_path.empty()) {
      report += "ERROR: Gameexe.ini or Seen.txt missing "
                "(case-corrected lookup also failed)\n";
      return NewSafeJavaString(env, report);
    }

    Gameexe gameexe(gameexe_path);
    // 上游 RLVMInstance 会写入 __GAMEPATH；资源查找层需要它。
    gameexe("__GAMEPATH") = dir;
    rlvm_android::SetGameFileSystem(
        rlvm_android::MakePosixGameFileSystem(dir));
    // 注意：这里**不**把存档指到游戏目录（SAF 侧）——存档菜单除了按槽位查
    // 存在性，还会用 boost 枚举整个存档目录，那条路径尚未接到 GameFileSystem，
    // 会出现"存得下但不显示"。指到普通路径（应用内）时四处都成立，故沿用默认。
    SetGameSaveDirectoryOverride(boost::filesystem::path());

    libreallive::Archive archive(seen_path.string(),
                                 gameexe("REGNAME").ToString(""));
    AndroidSystem system(gameexe);
    RunEngineOn(system, gameexe, archive, max_instructions, report);
    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "run report", report);
  return NewSafeJavaString(env, report);
}

/**
 * SAF 版本：全部文件都经由用户在系统选择器中授权的目录树访问，
 * 不拼接任何 File 路径（task.md 硬性要求）。
 *
 * Gameexe.ini 很小，整体读入后走 istream 构造；
 * SEEN.TXT 走 fd + mmap，避免把整个场景归档复制一遍。
 */
jstring RunScenarioSaf(JNIEnv* env, jobject /*thiz*/, jint max_instructions) {
  std::string report;

  // 同上：先拿到唯一运行权，再动 SAF 后端与引擎。
  RunGuard guard;
  if (!guard.acquired()) {
    report += "ERROR: 已有一台引擎在运行，请先点「停止引擎」。\n";
    LogReport(kLogTag, "saf run report", report);
    return NewSafeJavaString(env, report);
  }

  try {
    std::shared_ptr<rlvm_android::SafBackend> backend = rlvm_android::GetSafBackend();
    if (!backend) {
      report += "ERROR: no SAF backend installed\n";
      return NewSafeJavaString(env, report);
    }

    report += "SAF root listing:\n";
    std::vector<std::string> root_names;
    for (const auto& entry : backend->ListDirectory("")) {
      report += "  " + entry.name + (entry.is_directory ? "/" : "") + "\n";
      root_names.push_back(entry.name);
    }

    std::string gameexe_text;
    if (!rlvm_android::SafReadAll("Gameexe.ini", gameexe_text)) {
      report += "ERROR: cannot read Gameexe.ini via SAF\n";
      return NewSafeJavaString(env, report);
    }
    report += "Gameexe.ini read via SAF: " +
              std::to_string(gameexe_text.size()) + " bytes\n";
    std::istringstream gameexe_stream(gameexe_text);
    Gameexe gameexe(gameexe_stream);
    // SAF 下没有真实路径；__GAMEPATH 只在退化为普通路径后端时才会被使用。
    gameexe("__GAMEPATH") = std::string("saf:/");
    rlvm_android::SetGameFileSystem(rlvm_android::MakeSafGameFileSystem());
    // 同上：SAF 下暂不指定覆盖，继续用 $HOME/.rlvm/<REGNAME>/（真实路径，
    // 写入/读取/槽位检查/目录枚举四处都能工作）。
    SetGameSaveDirectoryOverride(boost::filesystem::path());
    AndroidSystem system(gameexe);

    // 资源查找层探针：走上游 System::FindFile 的完整链路
    //（读 #FOLDNAME -> 枚举目录 -> 扩展名匹配 -> 生成文件标识）。
    // 目标文件 g00/doesntmatter.g00 来自上游自带测试数据 test/Gameroot。
    {
      const boost::filesystem::path found =
          system.FindFile("doesntmatter", std::vector<std::string>{"g00"});
      if (found.empty()) {
        report += "FindFile(doesntmatter, g00) -> <not found>\n";
      } else {
        report += "FindFile(doesntmatter, g00) -> \"" + found.string() + "\"\n";
        std::shared_ptr<rlvm_android::GameFileSystem> vfs =
            rlvm_android::GetGameFileSystem();
        const int lookup_fd = vfs ? vfs->OpenFd(found.string()) : -1;
        report += std::string("  open via file system -> ") +
                  (lookup_fd >= 0 ? "fd=" + std::to_string(lookup_fd) : "FAILED") +
                  "\n";
        if (lookup_fd >= 0) close(lookup_fd);
      }
    }

    // 文本容器选择：默认游戏目录里的 Seen.txt；开关打开时改用用户指定的那个文件
    // （合并了汉化的容器）。指定的文件打不开就回退，避免"开关一开游戏直接起不来"。
    std::string container_path;
    {
      std::lock_guard<std::mutex> lock(g_text_container_mutex);
      container_path = g_text_container_path;
    }
    std::string container_label = "Seen.txt (游戏目录)";
    int fd = -1;
    if (!container_path.empty()) {
      fd = backend->OpenFd(container_path);
      if (fd >= 0) {
        container_label = container_path;
        report += "text container \"" + container_path +
                  "\" opened via SAF fd=" + std::to_string(fd) + "\n";
      } else {
        report += "WARNING: 指定容器 " + container_path +
                  " 打不开，回退到游戏目录 Seen.txt\n";
      }
    }
    if (fd < 0) {
      fd = backend->OpenFd("Seen.txt");
      if (fd < 0) {
        report += "ERROR: cannot open Seen.txt via SAF\n";
        return NewSafeJavaString(env, report);
      }
      report += "Seen.txt opened via SAF fd=" + std::to_string(fd) + "\n";
    }

    {
      // Archive 内部完成 mmap，之后即可关闭 fd（映射仍然有效）。
      libreallive::Archive archive(fd, "saf:/" + container_label,
                                   gameexe("REGNAME").ToString(""));
      close(fd);

      // 补丁机制：SEEN####.TXT 场景覆盖。
      // SAF 下没有可供 boost::filesystem 枚举的目录，因此这里用 SAF 后端
      // 列出文件名，再按名打开——文件名规则本身仍由 Archive 判定。
      const std::vector<std::string>& names = root_names;
      archive.ApplyOverrides(
          names, [&backend](const std::string& name) -> libreallive::Mapping* {
            const int override_fd = backend->OpenFd(name);
            if (override_fd < 0) return nullptr;
            libreallive::Mapping* mapping = nullptr;
            try {
              mapping = new libreallive::Mapping(override_fd, 0);
            } catch (...) {
              mapping = nullptr;
            }
            close(override_fd);
            return mapping;
          });

      int scenarios = 0;
      std::string indices;
      for (libreallive::Archive::const_iterator it = archive.begin();
           it != archive.end(); ++it) {
        ++scenarios;
        // 只列前 16 个：报告要逐行打印，索引全列会让开头几行淹没在噪声里。
        if (scenarios <= 16) {
          if (!indices.empty()) indices += ",";
          indices += std::to_string(it->first);
        }
      }
      if (scenarios > 16) indices += ",...";
      report += "Seen via SAF: TOC entries=" + std::to_string(scenarios) +
                "; indices=[" + indices + "]\n";
      // 诊断：引擎拿来解码文本的编码。0=CP932 1=CP936(GBK) 2=CP1252 3=CP949。
      report += "text encoding: probable=" +
                std::to_string(archive.GetProbableEncodingType()) + "\n";

      RunEngineOn(system, gameexe, archive, max_instructions, report);
    }

    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "saf run report", report);
  return NewSafeJavaString(env, report);
}

/** 由 Kotlin 侧在取得 SAF 目录授权后调用，安装 SAF 后端。 */
void SetSafBackendFromJava(JNIEnv* env, jobject /*thiz*/, jobject backend) {
  rlvm_android::InstallJniSafBackend(env, backend);
}

/**
 * 告知 native 侧诊断文件的所在目录（应用的外部文件目录）。
 *
 * 真机迭代一轮要重新构建 + 安装，很慢；把「跑多久 / 是否逐条追踪」这类
 * 观察参数放进目录下的 rlvm-diag.txt，用 adb push 改一行即可换一次实验。
 */
void SetDiagnosticsDir(JNIEnv* env, jobject /*thiz*/, jstring jdir) {
  std::lock_guard<std::mutex> lock(g_diag_mutex);
  g_diag_dir = JStringToUtf8(env, jdir);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "diagnostics dir = %s",
                      g_diag_dir.c_str());
  // 上游 System::GetHomeDirectory() 依赖 HOME/HOMEDRIVE/USERPROFILE，而 Android
  // 一个都没有——缺它会在"计算存档目录"时抛
  // "Could not find location of home directory."，存档与 Config 全部失败。
  // 这里给进程补一个可写的 HOME（应用外部文件目录），存档即可持久保存，
  // 且用户能在文件管理器里看到。
  if (!g_diag_dir.empty()) {
    setenv("HOME", g_diag_dir.c_str(), 1);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "HOME=%s", g_diag_dir.c_str());
  }
}

/**
 * 请求停止当前正在运行的引擎。
 *
 * 应用默认不限时运行（要能一直停在标题/正文上），所以必须有一个从 UI 喊停的通道：
 * 只置一个标志，引擎线程在下一轮循环开头看到后正常收尾（走完报告流程）。
 */
void RequestStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  g_stop_requested.store(true);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "stop requested");
}

/**
 * 挂起 / 恢复引擎（屏幕熄灭、应用进后台）。
 *
 * 与 requestStop 的区别：停止是「收尾退出」，挂起是「原地冻结」——进度、存档、
 * 音频位置全都不动，唤醒后接着跑。这里只置标志，真正的收敛点在引擎循环开头
 * （不推进指令、不合成帧）与 AudioEngine::SetSuspended（静音 + 停 AAudio 回调）。
 */
void SetEngineSuspended(JNIEnv* /*env*/, jobject /*thiz*/, jboolean suspended) {
  const bool value = suspended != JNI_FALSE;
  g_engine_suspended.store(value);
  rlvm_android::AudioEngine::Instance().SetSuspended(value);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "engine suspended=%s",
                      value ? "yes" : "no");
}

/**
 * 指定文本容器（汉化/原版的 SEEN 归档）在游戏目录里的相对路径，
 * 空串 = 回到游戏目录的 Seen.txt。
 *
 * 为什么要有这个开关：合并后的汉化容器是**另一个文件**，不能要求用户去覆盖游戏目录里
 * 的 SEEN.TXT（那是用户的原始文件，覆盖了就没法切回日文）。所以 UI 上给一个显式开关 +
 * 应用内文件列表，native 在开引擎时按这个相对路径走 SAF 打开容器；关掉即恢复原版。
 * 注意：游戏目录里的 Seen####.txt 覆盖文件仍然优先于容器（补丁机制的既有语义）。
 */
void SetTextContainerPath(JNIEnv* env, jobject /*thiz*/, jstring jpath) {
  const std::string path = jpath == nullptr ? std::string() : JStringToUtf8(env, jpath);
  {
    std::lock_guard<std::mutex> lock(g_text_container_mutex);
    g_text_container_path = path;
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "text container = %s",
                      path.empty() ? "(game dir Seen.txt)" : path.c_str());
}

/**
 * 触摸/鼠标输入（T2.3）。
 *
 * 坐标已经是**游戏帧坐标**（Kotlin 侧按帧在视图里的实际绘制矩形换算过），
 * 这里只做入队：真正的注入在引擎线程的 ExecuteEventSystem 里完成。
 */
void TouchEvent(JNIEnv* /*env*/, jobject /*thiz*/, jint action, jfloat x, jfloat y,
                jint buttons) {
  AndroidSystem* system = g_current_system.load();
  if (system == nullptr) return;  // 引擎没在跑，忽略

  // buttons 为 0 时退回诊断文件的缺省值（1=左键 2=右键 3=两者）。
  const int resolved = buttons != 0 ? static_cast<int>(buttons)
                                    : g_touch_buttons.load();
  static_cast<AndroidEventSystem&>(system->event())
      .PostTouchEvent(static_cast<int>(action),
                      Point(static_cast<int>(x), static_cast<int>(y)),
      resolved);
}

/**
 * 按键事件（v0.2.1 T7.2）。
 *
 * key_code 取 systems/base/event_listener.h 的 RLKEY_*（例如 LSHIFT=304、LCTRL=306、
 * UP=273）。与 TouchEvent 一样只入队，注入发生在引擎线程。
 *
 * 今天 RealLive 脚本还读不到键盘状态（RLVM 没有对应模块），这条通道先服务系统级消费者
 * （Shift/Ctrl）并为将来小游戏的 DLL 模拟预留。
 */
void KeyEvent(JNIEnv* /*env*/, jobject /*thiz*/, jint key_code, jboolean pressed) {
  AndroidSystem* system = g_current_system.load();
  if (system == nullptr) return;  // 引擎没在跑，忽略
  static_cast<AndroidEventSystem&>(system->event())
      .PostKeyEvent(static_cast<int>(key_code), pressed != JNI_FALSE);
}

/** 当前呈现帧的尺寸：高 16 位为宽、低 16 位为高；暂无帧时返回 0。 */
jint GetFrameSize(JNIEnv* /*env*/, jobject /*thiz*/) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame_width <= 0 || g_frame_height <= 0) return 0;
  return (g_frame_width << 16) | (g_frame_height & 0xFFFF);
}

/**
 * 当前帧序号（v0.2.2 性能修复）。
 *
 * GL 线程**每帧**都会问一次；只有序号变了才去拷贝 800x600x4 字节的像素。
 * 之前是每帧无条件 memcpy 1.9MB（120 次/秒 ≈ 230MB/s）并持有与引擎线程相同的
 * 互斥锁——引擎侧每次合成都要在锁上排队，实测把 7ms 的合成时间全耗在争用上，
 * 表现就是动画"被抽帧"。这里只读一个整数，代价可以忽略。
 */
jint GetFrameSerial(JNIEnv* /*env*/, jobject /*thiz*/) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  return static_cast<jint>(g_frame_serial);
}

/**
 * 把当前帧复制到调用方提供的直接缓冲区（宽*高 个 RGBA8888 像素）。
 * 返回帧序号；缓冲区过小或暂无帧时返回 -1。
 * 序号与上次相同表示没有新帧，GL 线程据此跳过重复上传。
 */
jint CopyFrameToBuffer(JNIEnv* env, jobject /*thiz*/, jobject buffer) {
  void* address = env->GetDirectBufferAddress(buffer);
  if (address == nullptr) return -1;
  const jlong capacity = env->GetDirectBufferCapacity(buffer);

  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame_pixels.empty()) return -1;
  const size_t bytes = g_frame_pixels.size() * sizeof(uint32_t);
  if (capacity < static_cast<jlong>(bytes)) return -1;

  std::memcpy(address, g_frame_pixels.data(), bytes);
  return static_cast<jint>(g_frame_serial);
}

const JNINativeMethod kNativeMethods[] = {
    {"versionString", "()Ljava/lang/String;", reinterpret_cast<void*>(VersionString)},
    {"probeAbi", "()I", reinterpret_cast<void*>(ProbeAbi)},
    {"probeGameDir", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(ProbeGameDir)},
    {"runScenario", "(Ljava/lang/String;I)Ljava/lang/String;",
     reinterpret_cast<void*>(RunScenario)},
    {"setSafBackend", "(Lorg/rlvm/android/SafFileSystem;)V",
     reinterpret_cast<void*>(SetSafBackendFromJava)},
    {"setDiagnosticsDir", "(Ljava/lang/String;)V",
     reinterpret_cast<void*>(SetDiagnosticsDir)},
    {"requestStop", "()V", reinterpret_cast<void*>(RequestStop)},
    {"setEngineSuspended", "(Z)V", reinterpret_cast<void*>(SetEngineSuspended)},
    {"setTextContainerPath", "(Ljava/lang/String;)V",
     reinterpret_cast<void*>(SetTextContainerPath)},
    {"touchEvent", "(IFFI)V", reinterpret_cast<void*>(TouchEvent)},
    {"keyEvent", "(IZ)V", reinterpret_cast<void*>(KeyEvent)},
    {"runScenarioSaf", "(I)Ljava/lang/String;",
     reinterpret_cast<void*>(RunScenarioSaf)},
    {"getFrameSize", "()I", reinterpret_cast<void*>(GetFrameSize)},
    {"getFrameSerial", "()I", reinterpret_cast<void*>(GetFrameSerial)},
    {"copyFrameToBuffer", "(Ljava/nio/ByteBuffer;)I",
     reinterpret_cast<void*>(CopyFrameToBuffer)},
};

/** 必须与 Kotlin 侧 org.rlvm.android.NativeBridge 完全一致。 */
constexpr char kBridgeClassName[] = "org/rlvm/android/NativeBridge";

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  // 尽早接管 stdout / stderr：上游 RLVM 的诊断信息全走这两个流，
  // 而 Android 应用默认把它们丢进 /dev/null。晚一步装就会漏掉早期输出。
  rlvm_android::InstallLogRedirect();

  // 安装"游戏文件标识 → fd"钩子：语音归档在 SAF 下靠它读文件（见上面说明）。
  SetOpenGameFileFdHook(&OpenGameFileFdHookImpl);
  // 写入变体：存档 / Config 落盘（SAF 下由 Kotlin 侧建文档）。
  SetOpenGameFileWriteFdHook(&OpenGameFileWriteFdHookImpl);

  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass bridge_class = env->FindClass(kBridgeClassName);
  if (bridge_class == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "FindClass(%s) failed", kBridgeClassName);
    return JNI_ERR;
  }

  const jint registered = env->RegisterNatives(
      bridge_class, kNativeMethods,
      static_cast<jint>(sizeof(kNativeMethods) / sizeof(kNativeMethods[0])));
  if (registered != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "RegisterNatives failed");
    return JNI_ERR;
  }

  // 保存 JavaVM：SAF 后端需要从 native 回调 Kotlin。
  rlvm_android::SetJavaVm(vm);

  __android_log_print(ANDROID_LOG_INFO, kLogTag, "native bridge registered");
  return JNI_VERSION_1_6;
}
