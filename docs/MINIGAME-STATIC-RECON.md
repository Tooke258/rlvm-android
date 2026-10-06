# 小游戏静态摸排（P2：丢背景 / 实体缺失 / 卡顿）

> 不接真机的静态分析记录。按 `TASKS.md` 的 P2 推进，发现即记；每条都要能指到
> 具体代码行 / 日志 / 反汇编。成对的过程记录在 `dev-log/MINIGAME-STATIC-RECON.jsonl`。

## 0. 本轮（2026-10-06 夜）已定位并修掉的两处

### 0.1 `ChildObjFg(2:81):1058` 的桩没覆写 `Dispatch`：每帧抛异常 + 效果全丢

* 现象：真机日志每帧刷
  `(SEEN7340)(Line 110)[lb_child_obj_1058]: Tried to call empty RLOp_SpecialCase::Dispatch().`
* 根因：`SEEN7340` line 110/111/145/146 是 `op<2:081:01058, 1>(203, 0, intL[0])` 这类
  子对象属性设置；这类 op 在 RLVM 里**不走** `RLOp_SpecialCase::DispatchFunction`，
  而是走 `module_obj.cc` 的 `handler->Dispatch(machine, currentInstantiation)`。
  我们的 `LbIgnoreRawArgs` 只覆写了 `ParseParameters` / `operator()`，于是命中基类
  `RLOp_SpecialCase::Dispatch` —— 而它的实现就是
  `throw Exception("Tried to call empty RLOp_SpecialCase::Dispatch().")`。
* 后果三连：① 每帧抛异常（异常远比一次函数调用贵）② 每帧一行 stdout（→ logcat 同步 I/O）
  ③ **这个 op 的效果从未落地**（203/202/205/204 号子对象的属性没被设置）。
* 修：`LbIgnoreRawArgs` 增加 `Dispatch` 覆写 = 真正的静默 no-op（一次性日志 + 推进 IP）。
  语义本身（1058 的 `(属性, 子序号, 值)` 怎么落到子对象）仍未实现，列在 P3。

### 0.2 每帧同步 stdout（卡顿的自身来源）

* `ObjBtnSelect`（`Sel 2:30/32`，小游戏**每帧**都走）里有三处无条件 `std::cout`，
  每次至少 4 行：两个 `objbtn hit#` + 一行 `objbtn poll`。
  这与之前 `pt00` 逐调用日志把游戏拖慢是同一类问题（logcat 同步 I/O）。
* 修：新增 `g_lb_verbose` + `LbLog()`（默认丢弃的 streambuf），三处改用 `LbLog()`；
  开关复用 diag 的 `pt00_verbose=1`。**默认关** → 小游戏循环里不再有每帧 I/O。

## 1. 小游戏高频 op 现状（SEEN7xxx 全段统计）

| op | 出现次数 | 我们这边的状态 |
| --- | --- | --- |
| `op<1:010:00000, 0>`（Bgr/Grp 载入） | 123606 | 已实现 |
| `op<0:003:00017, 0>`（Msg pause） | 105607 | 已实现（阻塞 op） |
| `op<0:001:00012, 1>`（farcall） | 43974 | 已实现 |
| `op<1:023:00000, 1>`（koePlay） | 41685 | 已实现 |
| **`op<1:004:00211, 0>`** | **37131** | **空实现（LbIgnoreRawArgs）** |
| **`op<1:004:00216, 0>`** | **37131** | **空实现** |
| **`op<1:004:00210, 0>`** | **7875** | **空实现** |
| **`op<1:004:00215, 0>`** | **7875** | **空实现** |
| `op<1:021:00000, 0/1>`（Pcm 族） | 3143 / 2172 | 部分（按名预载是空实现） |
| `op<2:081:01004, 0>`（ChildObjFg getter） | 1743 | 上游已实现 |
| `op<2:081:01058, 1>`（ChildObjFg 属性设置） | 见 §0.1 | 空实现（刚修掉异常，语义待补） |

**怀疑**：`Sys 210/211/215/216` 这一族（合计约 **9 万次**调用）是**等待/同步**类 op
（与 `Refresh`/帧计数同族），空实现会让时序不对 —— 既是"卡顿"也是"表现异常"的候选。
语义**必须**按 D-040 用真值标定后再实现（PC 侧 `input_calib.py` / A2 锚点 / 需要在原生引擎上
观察这四个 op 前后的帧计数与 intD 变化）。

## 2. 下一步（按性价比）

1. **量卡顿**：日志 I/O 已摘掉（§0.2），先看是否已经明显改善；再拆
   「执行器（每帧 CallDLL 次数 × 单次耗时）/ 渲染 / 合帧」三段（现成指标：
   `frame content hash=… n=<非黑像素>`、合帧耗时统计、`pt00` func 直方图）。
2. **丢背景**：`dump_graphics` 看该层在不在、src/dst 是否 0×0；再与 PC 同点截屏对照
   图层顺序（区分"没画"与"画了被盖"）。
3. **实体缺失**：按 A1 对象表逐项比对，重点看 children 与图层顺序；
   `1058` 语义补齐后重测（它可能正是"子对象属性没设上"的源头）。
4. **`210/211/215/216` 语义**：在 PC 上按"同一场景、同一帧区间"对照，看它们是否对应
   帧等待；确定后按真值实现。

> 维护：本文件随 P2 推进追加；新的成对记录写进 `dev-log/MINIGAME-STATIC-RECON.jsonl`。
