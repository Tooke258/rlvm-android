#ifndef PT00_EMU_BRIDGE_H_
#define PT00_EMU_BRIDGE_H_

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

// 逐调用诊断日志开关（默认关；logcat 是同步 I/O，开着会把小游戏越跑越慢）。
void SetVerbose(bool on);

// 卸载状态（换游戏目录时用）。
void Reset();

// 诊断：把脚本侧的 intD[0..1999] 整片打到 stderr（在 objbtn 诊断/图形栈转储那一刻调用，
// 用来和 PC 侧 pt00_probe 的 --full 输出逐项对照；小游戏的相机/相位差异都在这块数组里）。
void DumpIntD(class RLMachine& machine);

}  // namespace pt00emu

#endif  // PT00_EMU_BRIDGE_H_
