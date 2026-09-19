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
#include <mutex>
#include <sstream>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

#include <unistd.h>

// Boost 1.92 起 path.hpp / operations.hpp 不再传递包含 directory.hpp，必须显式引入。
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libreallive/archive.h"
#include "libreallive/gameexe.h"
#include "android/android_system.h"
#include "android/android_graphics.h"
#include "android/audio_engine.h"
#include "android/game_file_system.h"
#include "android/jni_saf_backend.h"
#include "android/log_redirect.h"
#include "android/saf_file_system.h"
#include "machine/game_hacks.h"
#include "machine/rlmachine.h"
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
// ---------------------------------------------------------------------------
std::mutex g_diag_mutex;
std::string g_diag_dir;

// 「停止引擎」请求。由 UI 线程置位，引擎线程在每轮循环开头检查。
// 应用默认持续运行（见 DiagOptions::time_budget_ms 的 0 = 不限时），
// 因此必须有一个从外部喊停的通道。
std::atomic<bool> g_stop_requested{false};

// 是否已有一台引擎在跑。默认不限时运行后，重复点「运行」很容易起第二台，
// 两台引擎共用同一个 AudioEngine 与帧缓冲会互相踩踏，所以直接拒绝并存。
std::atomic<bool> g_run_active{false};

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

struct DiagOptions {
  bool trace = false;
  bool dump_graphics = false;
  bool audio_selftest = false;
  // 0 表示不限时：应用要能一直停在标题/正文上，BGM 才不会「响一下就没了」。
  // 自动化测试需要在报告里拿到结果时，用 diag 文件设一个有限值。
  int time_budget_ms = 0;
  int max_instructions = 0;  // 0 表示沿用调用方传入的值
  int frame_log_every = 1;
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
    } else if (key == "time_budget_ms") {
      if (number > 0) options.time_budget_ms = number;
    } else if (key == "max_instructions") {
      if (number > 0) options.max_instructions = number;
    } else if (key == "frame_log_every") {
      if (number > 0) options.frame_log_every = number;
    }
  }
  return options;
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
  size_t non_black = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((g_frame_pixels[i] & 0x00FFFFFFu) != 0) ++non_black;
  }

  // 帧日志可以按间隔抽样：逐帧输出在长时间运行时会淹没真正重要的诊断信息。
  if (g_frame_log_every <= 1 ||
      (g_frame_serial % static_cast<unsigned int>(g_frame_log_every)) == 1u) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "frame %dx%d serial=%u nonblack=%zu/%zu", g_frame_width,
                        g_frame_height, g_frame_serial, non_black, count);
  }
}

/** 把 Java 字符串转成 UTF-8 的 std::string。 */
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
void RunEngineOn(System& system,
                 Gameexe& gameexe,
                 libreallive::Archive& archive,
                 int max_instructions,
                 std::string& report) {
  // 分步日志：真机上「跑很久却没有任何输出」时，用它定位卡在哪一步。
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: constructing RLMachine");
  RLMachine machine(system, archive);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddAllModules");
  AddAllModules(machine);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddGameHacks");
  AddGameHacks(machine);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: machine ready");
  // 与上游 RLVMInstance 一致：遇到指令异常时跳过该指令继续执行，
  // 而不是把整台机器标记为 halted。
  machine.SetHaltOnException(false);
  // 打印未实现的操作码与指令异常：上游默认把它们吞掉，
  // 在 Android 上 std::cerr 又看不到，于是表现为「跑了很多指令却什么都没发生」。
  machine.SetPrintUndefinedOpcodes(true);

  // 设备侧诊断参数（文件不存在时全部取缺省值，行为与之前一致）。
  const DiagOptions diag = LoadDiagOptions();
  g_frame_log_every = diag.frame_log_every;
  if (diag.max_instructions > 0) max_instructions = diag.max_instructions;
  if (diag.trace) machine.set_tracing_on();

  // REGNAME 是 CP932 编码的游戏数据，显示前先转成 UTF-8——
  // 否则报告本身含有非法 UTF-8 字节。
  report += "engine assembled (regname=\"" +
            ToDisplayUtf8(gameexe("REGNAME").ToString("")) + "\")\n";
  report += "diagnostics: trace=" + std::string(diag.trace ? "on" : "off") +
            " time_budget_ms=" + std::to_string(diag.time_budget_ms) +
            " max_instructions=" + std::to_string(max_instructions) +
            " frame_log_every=" + std::to_string(diag.frame_log_every) + "\n";

  // 重采样自检：把「音调是否偏高」变成日志里的一个频率数字。
  if (diag.audio_selftest) report += rlvm_android::ResamplerSelfTest();

  AndroidGraphicsSystem* graphics =
      dynamic_cast<AndroidGraphicsSystem*>(&system.graphics());

  // 与上游 RLVMInstance::Run 相同的结构：每轮先让子系统跑一遍（含合成一帧），
  // 再以 10ms 为时间片连续执行字节码。
  const unsigned int time_budget_ms = static_cast<unsigned int>(diag.time_budget_ms);
  const unsigned int started = system.event().GetTicks();
  // 只统计本次运行造成的合成量，先清零。
  TakeGraphicsBlitStats();
  g_stop_requested.store(false);
  int executed = 0;
  int frames_presented = 0;
  std::string stop_reason = "instruction budget exhausted";

  while (executed < max_instructions) {
    if (g_stop_requested.load()) {
      stop_reason = "stop requested";
      break;
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

    system.Run(machine);
    if (graphics != nullptr) {
      CaptureFrame(*graphics);
      ++frames_presented;
    }

    // 进度日志：定位「跑很久但没有输出」这类问题。
    if (frames_presented % 60 == 0) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "progress: frames=%d instructions=%d elapsed=%ums",
                          frames_presented, executed,
                          system.event().GetTicks() - started);
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
      machine.ExecuteNextInstruction();
      ++executed;
      now = system.event().GetTicks();
    } while (!machine.CurrentLongOperation() && !system.force_wait() &&
             (now - slice_start < 10));
    system.set_force_wait(false);
  }

  report += "instructions executed = " + std::to_string(executed) + "\n";
  report += "frames presented = " + std::to_string(frames_presented) + "\n";
  report += "stop reason = " + stop_reason + "\n";
  report += "halted = " + std::string(machine.halted() ? "yes" : "no") + "\n";
  const GraphicsBlitStats blits = TakeGraphicsBlitStats();
  report += "graphics blits: calls=" + std::to_string(blits.calls) +
            " written_pixels=" + std::to_string(blits.written_pixels) +
            " nonblack_pixels=" + std::to_string(blits.nonblack_pixels) + "\n";

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

    const int fd = backend->OpenFd("Seen.txt");
    if (fd < 0) {
      report += "ERROR: cannot open Seen.txt via SAF\n";
      return NewSafeJavaString(env, report);
    }
    report += "Seen.txt opened via SAF fd=" + std::to_string(fd) + "\n";

    {
      // Archive 内部完成 mmap，之后即可关闭 fd（映射仍然有效）。
      libreallive::Archive archive(fd, "saf:/Seen.txt",
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

/** 当前呈现帧的尺寸：高 16 位为宽、低 16 位为高；暂无帧时返回 0。 */
jint GetFrameSize(JNIEnv* /*env*/, jobject /*thiz*/) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame_width <= 0 || g_frame_height <= 0) return 0;
  return (g_frame_width << 16) | (g_frame_height & 0xFFFF);
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
    {"runScenarioSaf", "(I)Ljava/lang/String;",
     reinterpret_cast<void*>(RunScenarioSaf)},
    {"getFrameSize", "()I", reinterpret_cast<void*>(GetFrameSize)},
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
