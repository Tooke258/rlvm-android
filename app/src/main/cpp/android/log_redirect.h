// 把进程的 stdout / stderr 接到 logcat。
//
// 为什么需要：Android 应用的原生标准输出被系统丢进 /dev/null，而 RLVM 上游大量
// 诊断信息（未实现的操作码、被吞掉的指令异常、模块警告）都走 std::cout / std::cerr。
// 结果就是「跑了上千条指令却什么都看不到」，任何基于日志的判断都无从下手。
//
// 方案：在 fd 层面重定向，而不是替换 std::streambuf。
// 用管道接住 fd 1 / fd 2，再由读取线程逐行转发给 __android_log_print。
// 这样 printf、std::cout、std::cerr 以及任何直接 write(1, ...) 的代码都一并覆盖。

#ifndef RLVM_ANDROID_LOG_REDIRECT_H_
#define RLVM_ANDROID_LOG_REDIRECT_H_

namespace rlvm_android {

/**
 * 安装重定向。可重复调用，只有第一次生效。
 *
 * 应在 JNI_OnLoad 里尽早调用，早于任何可能输出诊断信息的 native 代码。
 * 日志标签：stdout -> `rlvm-stdout`（INFO），stderr -> `rlvm-stderr`（WARN）。
 */
void InstallLogRedirect();

}  // namespace rlvm_android

#endif  // RLVM_ANDROID_LOG_REDIRECT_H_
