// JNI 桥接层。
//
// task.md 硬性约束：所有 native 方法必须用 RegisterNatives 显式注册，不使用按符号名的动态查找。
// 因此本文件不声明 Java_org_rlvm_android_* 形式的导出函数，而是统一在 JNI_OnLoad 中绑定。

#include <jni.h>

#include <android/log.h>

#include <exception>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <chrono>
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
#include "android/saf_file_system.h"
#include "machine/game_hacks.h"
#include "machine/rlmachine.h"
#include "modules/modules.h"
#include "utilities/file.h"

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

  // 采样校验和：用于区分「引擎产出的帧本身是空的」与「呈现环节没显示出来」。
  uint64_t checksum = 0;
  for (size_t i = 0; i < count; i += 97) checksum += g_frame_pixels[i];
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "frame %dx%d serial=%u sampled_checksum=%llu", g_frame_width,
                      g_frame_height, g_frame_serial,
                      static_cast<unsigned long long>(checksum));
}

/** 把 Java 字符串转成 UTF-8 的 std::string。 */
std::string JStringToUtf8(JNIEnv* env, jstring value) {
  if (value == nullptr) return std::string();
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result = (chars != nullptr) ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
  return result;
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
      return env->NewStringUTF(report.c_str());
    }
    if (!fs::is_directory(root)) {
      report += "ERROR: not a directory\n";
      return env->NewStringUTF(report.c_str());
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

  __android_log_print(ANDROID_LOG_INFO, kLogTag, "probe report:\n%s", report.c_str());
  return env->NewStringUTF(report.c_str());
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
                 bool draw_bring_up_pattern,
                 std::string& report) {
  RLMachine machine(system, archive);
  AddAllModules(machine);
  AddGameHacks(machine);
  machine.SetHaltOnException(false);

  report += "engine assembled (regname=\"" + gameexe("REGNAME").ToString("") + "\")\n";

  AndroidGraphicsSystem* graphics =
      dynamic_cast<AndroidGraphicsSystem*>(&system.graphics());

  // 与上游 RLVMInstance::Run 相同的结构：每轮先让子系统跑一遍（含合成一帧），
  // 再以 10ms 为时间片连续执行字节码。
  const unsigned int time_budget_ms = 3000;
  const unsigned int started = system.event().GetTicks();
  int executed = 0;
  int frames_presented = 0;
  std::string stop_reason = "instruction budget exhausted";

  while (executed < max_instructions) {
    if (machine.halted()) {
      stop_reason = "machine halted";
      break;
    }
    if (system.event().GetTicks() - started > time_budget_ms) {
      stop_reason = "time budget exhausted";
      break;
    }

    system.Run(machine);
    if (draw_bring_up_pattern && graphics != nullptr)
      graphics->DrawBringUpPattern();
    if (graphics != nullptr) {
      CaptureFrame(*graphics);
      ++frames_presented;
    }

    if (machine.CurrentLongOperation()) {
      stop_reason = "entered long operation";
      break;
    }

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

  // 图像加载探针：走完整链路 System::FindFile -> GraphicsSystem::GetSurfaceNamed
  // -> AndroidGraphicsSystem::LoadSurfaceFromFile（OpenFd + GRPCONV 解码）。
  // 目标文件 g00/test.g00 由 tools/make_probe_fixture.ps1 生成（BMP 内容，
  // 因为解码器按内容而非扩展名分发）。
  if (graphics != nullptr) {
    try {
      std::shared_ptr<const Surface> image = system.graphics().GetSurfaceNamed("test");
      if (!image) {
        report += "image test -> <not loaded>\n";
      } else {
        int r = 0, g = 0, b = 0;
        image->GetDCPixel(Point(16, 16), r, g, b);
        report += "image test -> " + std::to_string(image->GetSize().width()) + "x" +
                  std::to_string(image->GetSize().height()) + "; pixel(16,16)=(" +
                  std::to_string(r) + "," + std::to_string(g) + "," +
                  std::to_string(b) + ")\n";
        // 画到帧缓冲并呈现，截图即可确认解码结果。
        const Rect full(Point(0, 0), image->GetSize());
        image->RenderToScreen(full, full, 255);
        CaptureFrame(*graphics);
      }
    } catch (const std::exception& e) {
      report += std::string("image test EXCEPTION: ") + e.what() + "\n";
    }
  }
}

jstring RunScenario(JNIEnv* env, jobject /*thiz*/, jstring jdir,
                    jint max_instructions) {
  namespace fs = boost::filesystem;
  std::string report;

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    const fs::path root(dir);
    const fs::path gameexe_path = CorrectPathCase(root / "Gameexe.ini");
    const fs::path seen_path = CorrectPathCase(root / "Seen.txt");

    if (gameexe_path.empty() || seen_path.empty()) {
      report += "ERROR: Gameexe.ini or Seen.txt missing "
                "(case-corrected lookup also failed)\n";
      return env->NewStringUTF(report.c_str());
    }

    Gameexe gameexe(gameexe_path);
    // 上游 RLVMInstance 会写入 __GAMEPATH；资源查找层需要它。
    gameexe("__GAMEPATH") = dir;
    rlvm_android::SetGameFileSystem(
        rlvm_android::MakePosixGameFileSystem(dir));

    libreallive::Archive archive(seen_path.string(),
                                 gameexe("REGNAME").ToString(""));
    AndroidSystem system(gameexe);
    RunEngineOn(system, gameexe, archive, max_instructions, true, report);
    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  __android_log_print(ANDROID_LOG_INFO, kLogTag, "run report:\n%s", report.c_str());
  return env->NewStringUTF(report.c_str());
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

  try {
    std::shared_ptr<rlvm_android::SafBackend> backend = rlvm_android::GetSafBackend();
    if (!backend) {
      report += "ERROR: no SAF backend installed\n";
      return env->NewStringUTF(report.c_str());
    }

    report += "SAF root listing:\n";
    for (const std::string& name : backend->ListDirectory("")) {
      report += "  " + name + "\n";
    }

    std::string gameexe_text;
    if (!rlvm_android::SafReadAll("Gameexe.ini", gameexe_text)) {
      report += "ERROR: cannot read Gameexe.ini via SAF\n";
      return env->NewStringUTF(report.c_str());
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
      return env->NewStringUTF(report.c_str());
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
      const std::vector<std::string> names = backend->ListDirectory("");
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
        if (scenarios <= 8) {
          if (!indices.empty()) indices += ",";
          indices += std::to_string(it->first);
        }
      }
      report += "Seen via SAF: TOC entries=" + std::to_string(scenarios) +
                "; indices=[" + indices + "]\n";

      RunEngineOn(system, gameexe, archive, max_instructions, true, report);
    }

    // 音频探针：经 SAF 打开 test.wav（3 秒 440Hz 正弦），用 AAudio 播放 3 秒后
    // 汇报回调次数、渲染帧数与峰值。峰值非零说明真的把非静音数据送进了设备。
    {
      const int audio_fd = backend->OpenFd("test.wav");
      if (audio_fd < 0) {
        report += "audio: test.wav not found via SAF\n";
      } else {
        rlvm_android::AudioEngine& audio = rlvm_android::AudioEngine::Instance();
        if (!audio.Start()) {
          report += "audio: start failed: " + audio.LastError() + "\n";
        } else {
          std::unique_ptr<rlvm_android::AudioSource> source =
              audio.OpenSource(audio_fd, "wav");
          if (!source) {
            report += "audio: decode failed\n";
          } else {
            audio.ResetPeak();
            audio.Play(0, std::move(source), false, 255);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            const rlvm_android::AudioEngine::Stats stats = audio.GetStats();
            report += "audio: callbacks=" + std::to_string(stats.callbacks) +
                      " frames=" + std::to_string(stats.frames_rendered) +
                      " peak=" + std::to_string(stats.peak_amplitude) + "\n";
            audio.Stop(0);
          }
        }
      }
    }
    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  __android_log_print(ANDROID_LOG_INFO, kLogTag, "saf run report:\n%s", report.c_str());
  return env->NewStringUTF(report.c_str());
}

/** 由 Kotlin 侧在取得 SAF 目录授权后调用，安装 SAF 后端。 */
void SetSafBackendFromJava(JNIEnv* env, jobject /*thiz*/, jobject backend) {
  rlvm_android::InstallJniSafBackend(env, backend);
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
