// JNI 桥接层。
//
// task.md 硬性约束：所有 native 方法必须用 RegisterNatives 显式注册，不使用按符号名的动态查找。
// 因此本文件不声明 Java_org_rlvm_android_* 形式的导出函数，而是统一在 JNI_OnLoad 中绑定。

#include <jni.h>

#include <android/log.h>

#include <exception>
#include <sstream>
#include <string>

#include <unistd.h>

// Boost 1.92 起 path.hpp / operations.hpp 不再传递包含 directory.hpp，必须显式引入。
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libreallive/archive.h"
#include "libreallive/gameexe.h"
#include "android/android_system.h"
#include "android/jni_saf_backend.h"
#include "android/saf_file_system.h"
#include "machine/game_hacks.h"
#include "machine/rlmachine.h"
#include "modules/modules.h"

namespace {

constexpr char kLogTag[] = "rlvm-native";

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
      report += "  " + p.filename().string() + "  " +
                std::to_string(fs::file_size(p)) + " bytes\n";
    }

    // --- Gameexe.ini ---
    const fs::path gameexe_path = root / "Gameexe.ini";
    if (fs::exists(gameexe_path)) {
      // 注意：Gameexe 位于全局命名空间，只有 Archive / Scenario 在 libreallive 里。
      Gameexe gameexe(gameexe_path);
      const std::string caption = gameexe("CAPTION").ToString("");
      report += "Gameexe: parsed OK";
      if (!caption.empty()) report += " (CAPTION=" + caption + ")";
      report += "\n";
    } else {
      report += "Gameexe: Gameexe.ini not found\n";
    }

    // --- SEEN.TXT ---
    const fs::path seen_path = root / "Seen.txt";
    if (fs::exists(seen_path)) {
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
      report += "Seen: Seen.txt not found\n";
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
void RunEngineOn(Gameexe& gameexe,
                 libreallive::Archive& archive,
                 int max_instructions,
                 std::string& report) {
  AndroidSystem system(gameexe);

  RLMachine machine(system, archive);
  AddAllModules(machine);
  AddGameHacks(machine);
  machine.SetHaltOnException(false);

  report += "engine assembled (regname=\"" + gameexe("REGNAME").ToString("") + "\")\n";

  int executed = 0;
  std::string stop_reason = "instruction budget exhausted";
  while (executed < max_instructions) {
    if (machine.halted()) {
      stop_reason = "machine halted";
      break;
    }
    machine.ExecuteNextInstruction();
    ++executed;
    if (machine.CurrentLongOperation()) {
      stop_reason = "entered long operation";
      break;
    }
  }

  report += "instructions executed = " + std::to_string(executed) + "\n";
  report += "stop reason = " + stop_reason + "\n";
  report += "halted = " + std::string(machine.halted() ? "yes" : "no") + "\n";
}

jstring RunScenario(JNIEnv* env, jobject /*thiz*/, jstring jdir,
                    jint max_instructions) {
  namespace fs = boost::filesystem;
  std::string report;

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    const fs::path root(dir);
    const fs::path gameexe_path = root / "Gameexe.ini";
    const fs::path seen_path = root / "Seen.txt";

    if (!fs::exists(gameexe_path) || !fs::exists(seen_path)) {
      report += "ERROR: Gameexe.ini or Seen.txt missing\n";
      return env->NewStringUTF(report.c_str());
    }

    Gameexe gameexe(gameexe_path);
    libreallive::Archive archive(seen_path.string(),
                                 gameexe("REGNAME").ToString(""));
    RunEngineOn(gameexe, archive, max_instructions, report);
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

      RunEngineOn(gameexe, archive, max_instructions, report);
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
