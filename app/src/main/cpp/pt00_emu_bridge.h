#ifndef PT00_EMU_BRIDGE_H_
#define PT00_EMU_BRIDGE_H_

#include <string>

class RLMachine;

// PT00 小游戏的**兼容层**桥：把脚本的 CallDLL 转给自研 x86-32 执行器，
// 由它直接执行**用户游戏数据里**的原版 PT00.dll（DLL 不进仓库/APK）。
// 设计依据与验收：docs/PT00-EMU-HANDOFF.md（calls_long 280/280 逐位一致）。
//
// 拿不到 DLL、或执行器装载失败时一律返回 false，由调用方退回行为级重写，
// 保证没放 DLL 的用户不会因为兼容层而崩。
namespace pt00emu {

// 返回 true 表示这次 CallDLL 已由执行器处理。
bool CallDLL(RLMachine& machine, int func, int a1, int a2, int a3, int a4);

// 诊断用：兼容层是否已经就绪（供日志/自检）。
bool Available();

// 取证：记录执行器对 ctx 块与低地址的读写（小游戏卡住时看 DLL 在找哪个引擎数组）。
void SetTraceCtx(bool on);

// 试验开关：每帧首（脚本调 72(0) 时）替脚本补调一次 CallDLL(0,31)「每帧主推进」。
void SetTick31(bool on);

// pt00_intg_hack=0：不在首次调用 DLL 时预置 intG[1900]/[1901]（交给脚本置位）。
void SetIntgHack(bool on);

// 观察点：执行器每次 CallDLL 第一次命中 eip 时打印寄存器 + [edi-0x10] 结构体窗口。
// 用途见 native-bridge.cpp 的 pt00_watch 注释（拿回死循环入口的 begin/end）。
void SetWatch(unsigned eip, int max_lines);
void SetFpTrace(unsigned lo, unsigned hi, int cap);
void SetInsnTrace(unsigned lo, unsigned hi, int cap);
void SetJccFlip(unsigned eip);
// 按 guest 地址盯几个 dword（diag pt00_peek=10023D20,10023D28），值变化时打一行。
void SetPeek(const std::string& csv_hex);
// CRT 静态构造（diag pt00_run_ctors=1）。
void SetRunCtos(bool on);
// diag intd_pin=<slot>=<value>[,…]：每次喂 intD 快照给 DLL 时钉住这些槽。
void SetIntdPin(const std::string& csv);
// diag intd_bias=<slot>=<delta>[,…]：喂给 DLL 时给这些槽加常数（保留脚本推进）。
void SetIntdBias(const std::string& csv);

// 逐调用诊断日志开关（默认关；logcat 是同步 I/O，开着会把小游戏越跑越慢）。
void SetVerbose(bool on);

// 卸载状态（换游戏目录时用）。
void Reset();

// 诊断：把脚本侧的 intD[0..1999] 整片打到 stderr（在 objbtn 诊断/图形栈转储那一刻调用，
// 用来和 PC 侧 pt00_probe 的 --full 输出逐项对照；小游戏的相机/相位差异都在这块数组里）。
void DumpIntD(class RLMachine& machine);

}  // namespace pt00emu

#endif  // PT00_EMU_BRIDGE_H_
