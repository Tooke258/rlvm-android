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
// 逐指令 trace 的打印预算（行数）。-1 = 不限，0 = 关闭，>0 = 还能打这么多行。
void SetLbOpTraceBudget(long n);
// 死循环探测（diag: loop_detect=1）。每次派发指令时调用 NoteOpForLoopDetect()：
// 一旦发现"最近 8 条 (场景,行号) 序列按周期 1..4 完全重复"，就写一行
// `loop_detector: hot loop ...` 到应用日志（只报一次），不走 logcat 环形缓冲。
// 用途：像 Scene 回想这种"卡死但不崩"的问题，直接告诉我们是哪一行在空转。
void SetLoopDetector(bool on);
void NoteOpForLoopDetect(int scene, int line);
// 长操作（"等一等"）日志（diag: longop_log=1）：push/pop 各写一行。
void SetLongOpLog(bool on);
bool LongOpLogWanted();

// goto_case / goto_on 的**分派取证**（diag: case_trace=1，默认关）：
// 每次求值都打一行 `[case] SEENxxxx Lnnn value=? n=? [i](case)… -> idx=?`。
// 用途：LBEX 暂停菜单里 `goto_case(intA[i])` 选图案号那一处，确认「求值多少、
// 命中哪个 case、有没有走默认」。命中失败会抛 "no default case"。
void SetLbCaseTrace(bool on);
bool LbCaseTraceWanted();

// 图案号写入取证（diag: patno_trace=1，默认关）：每次 `objPattNo`（`2:81:1039` 等）
// 打一行 `[patno] SEENxxxx Lnnn parent=? child=? set=? now=?`。
// 用途：暂停菜单图标全是 0 号脸时，确认「写入有没有发生 / 值对不对 / 写完立刻读回多少」。
void SetLbPatNoTrace(bool on);
bool LbPatNoTraceWanted();

// 相册页码取证（diag: gallery_probe=1，默认关）：`SEEN9515` 的
// L278（取页码表）/ L301（守卫）/ L304·L307·L310（写 objPattNo）几行，
// 每次派发打一行 `[gallery] SEEN9515 Lnnn intL[0]=.. intL[11]=.. intL[20]=.. intL[21]=.. intA[7200+intL[11]]=..`。
// 用途：页码图案号变 0 时，判断是"页码源本身是 0/-1"还是"守卫没过、屏幕上是残留"。
void SetLbGalleryProbe(bool on);
bool LbGalleryProbeWanted();

// 复位抑制实验（diag: wipe_copy_all=1，默认关）：让 `ClearAndPromoteObjects()` 把
// **所有**前景槽都当成打过 objFgWipeCopyOn（即跳过 `fg->InitializeParams()/FreeObjectData()`）。
// 用途：验证「UI 先正确渲染一瞬、随后复位默认」到底是不是这次复位在承载。
// 这不是修法，是机械试验——打开后普通场景会留残影（对象不再被清）。
void SetLbWipeCopyAll(bool on);
bool LbWipeCopyAllWanted();

// 按钮覆盖抑制（diag: no_button_overrides=1，默认关）：让
// `ButtonObjectSelectLongOperation::SetButtonOverride()` 直接不生效 ——
// 于是按钮对象渲染时用脚本写的 `patt_no_`，而不是 `GAMEEXE.INI` 的 `BTNOBJ.ACTION` 表。
// 用途：验证「UI 全部照默认图像渲染」是不是这张表/这条覆盖链造成的（PC 汉化版缺键/错值）。
void SetLbNoButtonOverrides(bool on);
bool LbNoButtonOverridesWanted();

// 「最近一次派发的指令」上下文（scene/line/op 名），给 graphics_object 侧的
// `InitializeParams()` 取证用：`[params-reset] after=SEENxxxx Lnnn op`。
void SetLbLastOpContext(int scene, int line, const std::string& op_name);
std::string LbLastOpContextString();

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_APP_LOG_H_
