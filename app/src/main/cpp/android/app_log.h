// 应用日志文件（v0.2.3）
//
// 为什么需要：真机上看不到 logcat（config GUI 迭代后 App 面板里的日志是唯一入口），
// 而 native 侧的关键事件（影片起播/解码结束、熔断等）原本只进 logcat，用户看不到。
// 这里提供一个「同时写 logcat + 追加到 <外部文件目录>/rlvm-log.txt」的入口，
// Kotlin 侧的 log() 也写同一个文件，面板里的「日志」视图直接显示它。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_APP_LOG_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_APP_LOG_H_

#include <string>

namespace rlvm_android {

// 应用启动时告诉 native 日志文件放哪（一般是 <外部文件目录>/rlvm-log.txt）。
void SetAppLogFile(const std::string& path);

// 写一行：照旧进 logcat，同时追加到上面的日志文件。
void AppendAppLogLine(const std::string& line);

// 诊断开关：对象「晋升/擦除」（GraphicsSystem::ClearAndPromoteObjects）明细。
// diag 键 wipe_log=1 打开。默认关——它会逐对象打行，只在排查渲染丢失时开。
bool LbWipeLogEnabled();
void SetLbWipeLog(bool on);

// 逐指令 trace 的**白名单**过滤器（diag 键 op_trace=a,b,c）。
// 过滤器为空时 = 不过滤（此时只有 trace=1 才有输出，行为与原来一致）。
// 非空时：只有名字里含任一子串的指令才会被打印——用来在小游戏里只盯
// 「可能改对象状态」的那十来种指令，避免 46MB 的全量 trace。
void SetLbOpTraceFilter(const std::string& csv_substrings);
bool LbOpTraceWanted(const std::string& op_name);

// goto_case / goto_on 的**分派取证**（diag: case_trace=1，默认关）：
// 每次求值都打一行 `[case] SEENxxxx Lnnn value=? n=? [i](case)… -> idx=?`。
// 用途：LBEX 暂停菜单里 `goto_case(intA[i])` 选图案号那一处，确认「求值多少、
// 命中哪个 case、有没有走默认」。命中失败会抛 "no default case"。
void SetLbCaseTrace(bool on);
bool LbCaseTraceWanted();

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_APP_LOG_H_
