# PT00 兼容层 · 交接文档

> 生成时间：2026-10-05（用户手动压缩上下文前的固化点）
> 用途：换人 / 压缩上下文后，靠这一份就能接着干，不必重新逆向。
> 配套阅读：`docs/PT00-CALLSITES.md`（调用面取证 M1）、`docs/PT00-TOOLCHAIN.md`（三层工具链）、
> `docs/LB-MINIGAME-RETRO.md`（卡 TIME 复盘）、`dev-log/PT00-EMU.jsonl`（逐轮日志）。

## 1. 一句话现状

**PT00（LB/LBEX 棒球小游戏）的路线已从「手写重写」切换为「兼容层」：在 Android 侧跑一个自研的
x86-32 执行器，直接执行原版 `PT00.dll`。** 执行器在 PC 侧已经能跑通原版 DLL，
最小夹具 **7/7 逐位一致**，长序列夹具（280 次调用）**首处差异收敛到第 10 次调用 `func 71(1)`**。
剩余工作 = 收掉 `func 71` 的类型分派 + 把执行器接进 APK，之后才需要真机实测。

## 2. 决策与边界（不要忘）

| 项 | 结论 |
| --- | --- |
| 路线 | 手写重写 → **兼容层（跑原版 DLL）**。触发点：用户判断小游戏各链路强耦合，继续猜没有收敛性 |
| 收益证据 | 已抓到的 9 个 bug **全部在宿主执行器**，`PT00.dll` 一行没改（见 §6） |
| 许可边界 | 原版 `PT00.dll` **由用户游戏数据在运行时提供，绝不进仓库、不进 APK**；执行器是自研 GPLv3 代码 |
| 库选择 | **不用 Unicorn / QEMU**（GPLv2-only，与 GPLv3 不兼容）。执行器 ~1300 行 C，自己写 |
| 与 RLVM 上游的关系 | 不改 `libreallive` / `machine` / `modules` 语义；接入点在平台层 `LittleBustersPT00DLL::CallDLL()` |

## 3. 静态规模（已量死，别重复量）

来源：`llvm-objdump` 指令直方图 + IDA 函数表可达性分析（原始记录见 `dev-log/PT00-EMU.jsonl`）。

| 指标 | 值 |
| --- | --- |
| 全 DLL | 41014 条指令 / 177 种助记符 |
| 浮点 | 833 条，**全部是 x87，无一条 SSE/SSE2**（SSE2 才是模拟器的大坑，这里不存在） |
| 函数总数 | 426 |
| 四个导出可达的函数 | **71 个**（含 `DllEntryPoint` 共 72） |
| 可达代码 | **4897 条指令 / 82 种助记符** |
| 冷门指令 | `aaa/aad/arpl/inb/outb/insb/wbinvd/hlt/cli/std/ljmp` 全部落在**不可达区** |

→ 工作量收敛成「约 80 条助记符 + 一套 CRT shim」，不是「模拟一台 PC」。

## 4. 已验证的调用契约（写代码必须照这个来）

### 4.1 入口

```
reallive_dll_func_load(ctx, a2)   // ctx 偏移 +0x14 处放 intD 基址
reallive_dll_func_call(func, a1..a4)
```

* 导出只有这两个（`func_call` 内部再按 `func` 分发）。`PT00.dll` **只导入 KERNEL32**，没有 GDI/USER32/输入。
* PC 上 `pt00.dll` 加载在 **`0x10000000`**（未重定位）；执行器按同样基址装载。
* 所有状态交换**只走 intD 区**：参数全是 int，没有指针 / 内存块参数；脚本也**不看返回值**
  （每条出口都 `return 1`，全库 0 处 `<store>`）。

### 4.2 `func_load` 的副作用（已对账）

| 位置 | 值 |
| --- | --- |
| `ctx[0x10023D18]` | 传入的 `ctx` |
| `entities[0x100237C4]` | 22 个堆指针（实体对象） |
| 堆已用 | **≈27968 = 22 × 1272**（每实体 1272 字节） |
| 返回 | `1` |

### 4.3 对象 ↔ intD 的桥（`sub_10004B40` = vtable idx1，加载时调用）

| 槽 | 值 |
| --- | --- |
| `obj[1]` | intD 基址 |
| `obj[2]` | `&intD[1000 + 36*index]`（该实体的记录） |
| `obj[3]` | 基址 + 840 = `&intD[210]` |
| `obj[4]` | 基址 + 1000 = `&intD[250]` |
| `obj[312]` | 实体下标 |

PC 实测自检：`obj[0]=0x1001D2A8`（该实体的 vtable）、`obj[1]=0x01484F1C`。

### 4.4 实体 vtable 与分派

* 实体 vtable：实体 0 → `0x1001D2A8`、实体 1 → `0x1001D284` …（每种实体一个）。
  槽位布局（实测 dump）：`+0x00` dtor `10001610`／`+0x04` idx1 `10004b40`（绑 intD）／
  `+0x08` idx2 `10004b80`(func 70)／`+0x0C` idx3 `100050d0`(func 71)／`+0x10` idx4 `100052f0`／
  `+0x14` idx5 `10005330`(设模式)／`+0x18` idx6／`+0x1C` idx7／`+0x20` idx8（后三个按类型不同）。
  例：实体 2 = `0x1001D260`，idx6/7/8 = `10008ea0 / 10008ee0 / 10008e50`；
  实体 1 = `0x1001D284`，idx6/7/8 = `10007360 / 100073f0 / 100079e0`。
  **实体 2 的 idx8 就是一个 6 路的「模式 → 处理体」跳转表**（`10008e57: cmpl $0x5`，
  表在 `0x10008e84`：→ `10009200 / 10009230 / 10009340 / 100094d0 / 10009660 / 100096f0`），
  索引来自记录里的 `+0x0C`（= `record[3]`，即"子模式"）。
* **idx2(func 70) / idx3(func 71) / idx5(模式设置) 在所有 22 种实体上是同一实现**，类型差异只在 idx6/7/8。
* `reallive_dll_func_call` 的分派：`eax = func - 10` → 范围检查 → 查索引字节表 `0x10001958`
  → `jmp *0x10001924(,%ecx,4)`。
* `pt00_prefix_sum_table`(`0x100022D0`)：增量表 → 前缀和，参数 `(dst, src, count)`，`count` 来自 `.data` 全局。

### 4.5 时间源必须确定性化

`func_load` 开头有 `time(&t); srand(t);`，而 `func 71` 的类型行为里用 `rand()`。
`oracle.c` 已把 IAT 的 `GetSystemTime` / `GetLocalTime` / `GetTimeZoneInformation` **改写成确定性全 0 桩**，
与执行器对齐。**其余 func 不用 rand，所以早期 7/7 全对**；不对齐时间源会让两边随机序列从第一个 `rand` 起分叉。

> 铁律：**对照两侧必须同种子（`--seed`）**。早期忘了同种子时出现的「184 处差异」全是假警报。

## 5. 三个工具（都在 PC 侧，不需要真机）

| 工具 | 作用 |
| --- | --- |
| `tools/pt00_oracle/oracle.c` | **真值**：`LoadLibrary` 原版 DLL，逐调用打印 intD 改动；`--seed` 对齐起点；时间源已确定性化 |
| `tools/pt00_emu/emu.c` | **执行器**：PE 装载 + 整数/内存栈 + x87 + 导入桩（IAT 改写 + cdecl 拦截）+ TIB 影子（`fs:`）+ 调用栈/栈平衡断言 + trace，约 1300 行 C |
| `tools/pt00_oracle/compare.py` | 同序列同种子跑两边，**逐位对照**，退出码 0/1 可当门禁 |

辅助脚本：`tools/pt00_oracle/make_fixtures.py` 生成夹具（`seed.bin` = intD[2000]+intF[2000] 小端 32 位；
`calls_long.txt` = 280 次调用 / 8 帧真实形态）。

### 命令（在仓库根执行）

```powershell
tools\pt00_oracle\build.bat            # 32 位 oracle（x86 工具链）
tools\pt00_emu\build.bat               # 执行器
python tools\pt00_oracle\make_fixtures.py                       # -> build\pt00-fixtures\
python tools\pt00_oracle\compare.py "<PT00.dll>" tools\pt00_oracle\calls_sample.txt
python tools\pt00_oracle\compare.py "<PT00.dll>" build\pt00-fixtures\calls_long.txt --seed build\pt00-fixtures\seed.bin
tools\pt00_emu\emu.exe "<PT00.dll>" build\pt00-fixtures\calls_long.txt --seed build\pt00-fixtures\seed.bin --trace-func 71 --trace-min 10007360
```

* `build/` 与 `tools/pt00_*/*.exe`、`*.obj` 都**不入库**（见 `.gitignore`）。
* **Windows 侧编 C 必须 `cl /utf-8`**：MSVC 默认按 CP936 读 UTF-8 源，中文注释会被错解并**吞掉后面的代码**
  （表现为莫名其妙的语法错误）。两个 `build.bat` 已固化该开关。
  另外 **`.bat` 本身必须是 ASCII**（或带 BOM）：UTF-8 + 纯 LF 的 `build.bat` 会被 cmd 按 CP936 解析而吞掉换行
  （见 §6.1 第 19 条）。
* trace 语义：`--trace-func N` 只在**第一次**匹配的调用处武装（要追第 N 次得自己裁 calls 文件或加 `--trace-call`）；
  `--trace-min <hex>` 只打 `eip ≥ 该地址`；开 trace 后每次写 intD 都会打出 `WRITE intD[k]=v @写入指令地址`——定位「谁写错」最快。

## 6. 已修的执行器 bug 清单（收敛梯度就在这份清单上）

1. **SIB 的 `base==5 && mod==0` = disp32，与 scale 无关**（多写了 `(sib>>6)==0` 条件）
   → `jmpl *table(,%ecx,4)` 跳转表被当成 base=ebp → 跳到栈上执行 → 「函数只跑十几条就返回」。
   *这是最关键的一条。*
2. KERNEL32 是 **stdcall（被调方清栈）**，桩必须 `esp += argc*4`，否则栈每调用漂 4 字节，最后 `ret` 弹出假地址。
3. 8 位 ALU 族（`xor r8, r/m8` 等）没实现。
4. `F7 /5` 单操作数 `imul`（除 10 魔数的一半）。
5. **x87 状态字 C0/C2/C3 没实现** → `fnstsw ax` 恒 0 → `fcompi / fnstsw / test ah,0x41` 这类浮点比较分支永远走同一边。
6. **x87 寄存器形式要用原始 ModRM 字节当操作码**（`d9 ff` = `fcos`），只存 rm 字段会一条都匹配不上。
7. `rep movs/stos` 族（内联 memcpy/memset）。
8. **`fistpll`(DF /7) 是 8 字节 int64**（按 int16 写会让高位读到旧垃圾，整数结果变成 `0xEFDFxxxx`）；
   顺带补了 **`fldcw` 舍入控制**（`_ftol2` 会先 `orb $0xc,%ah` + `fldcw` 切到向零取整）。
9. **`inc/dec r32`(0x40–0x4F) 没把结果写回寄存器**（只算标志）
   → `decl` 后模式号不变 → switch 跳转表索引整体偏移一格 → idx8 的 case 1 走进 case 2 body。

### 6.1 第二批（2026-10-05 续，把长序列里的「未实现指令」清干净）

10. **`emu.c` 从来没 `#include <math.h>`** → `sqrt/sin/cos/pow/atan2/tan/fmod/log` 全被隐式声明成 `int`，
    整条浮点路径系统性算错。*这一条影响最大*，加完 include 后 `71(1)` 立刻对齐。
11. `test r/m8, r8`（**0x84**）缺失 —— 实体 1 起的 idx7 路径直接报未实现并中断
    （表现：`ret` 留下残留指针、intD 一处不写）。**这就是当时「71(1) 全无输出」的真因。**
12. `F6` 族（8 位 `test/not/neg/mul/imul/div/idiv`）缺失 —— `negb %al` 就在里面。
13. `A8/A9`（`test al/eax, imm`）缺失（`strcpy` 那类 4 字节扫描循环会用到）。
14. `sbb`（**0x19/0x1b**）缺失 —— `sbbl %eax,%eax`（把 CF 变成 0/-1）是常见写法。
15. x87 补一批：`DC /2,/3`（fcom/fcompl m64）、`DD` 寄存器形式（fst/fstp st(i)）、
    `DA E9`（fucompp）、`DB E2/E3`（fnclex/fninit）、超越函数
    `fpatan/fptan/f2xm1/fyl2x/fyl2xp1/fsincos/frndint/fprem/fscale`，以及
    `fldpi/fldl2t/fldl2e/fldlg2/fldln2` 常数。
16. `div/idiv` 只用了 `EAX`，**没把 `EDX:EAX` 当 64 位被除数**。
17. **TLS**：`TlsGetValue` 之前恒返回 0 → CRT 的 `__getptd()` 每次 `rand()` 都重建 ptd
    （**rand 状态就存在 `ptd+0x14`**）→ 随机序列与原生分叉。现在按槽持久化（`tls_slot()`）。
18. `GetModuleFileNameA` 桩缺失（CRT 会拿它做初始化，返回空串更容易踩怪路径）。
19. **`tools/pt00_*\\build.bat` 两个文件是 UTF-8 + 纯 LF**：cmd 按 CP936 解析时中文注释会吞掉换行、
    把两条命令并成一条 —— 报错长这样：`'l.bat' is not recognized`、`'" x64 >nul' is not recognized`。
    已改成**纯 ASCII + vswhere 兜底 + `cd /d "%~dp0"`**（仓库内 `.bat` 以后一律 ASCII）。

诊断增强（同样是这轮）：`UNIMPL` 现在报**指令起始地址 + 原始字节 + 最近 32 条指令**；
步数上限与 `ExitProcess` 都会打**调用栈**（`callee<-caller_ret`）。

## 7. 当前对照状态与「下一处」原始证据

`2026-10-05` 实测（提交 `2f4b79f` 之后）：

| 夹具 | 结果 |
| --- | --- |
| `tools/pt00_oracle/calls_sample.txt`（7 次：30/31/10/12/920/921/60） | **7/7 逐位一致** ✓ |
| `build/pt00-fixtures/calls_long.txt`（280 次，8 帧真实形态） | 前 9 次一致；**首处差异 = 第 10 次调用 `71(1)`** |

首处差异的原始输出（`compare.py --limit 30`）：

```
第 1 处不一致：
   oracle: 10 f=71(1,0,0,0) ret=1 intD[1040]=13 intD[1041]=1 intD[1046]=-15 intD[1049]=6
                               intD[1053]=-1400 intD[1060]=100 intD[1061]=125 intD[1062]=1
                               intD[1064]=1 intD[1067]=1
   emu   : 10 f=71(1,0,0,0) ret=272633856            ← 0x10401000，intD 一处都没写
第 2 处不一致：
   oracle: 11 f=71(2,...) ret=1 intD[1076]=1 intD[1077]=1 intD[1096]=100 intD[1098]=1 intD[1100]=1 intD[1103]=1
   emu   : 11 f=71(2,...) ret=272633856 intD[1076]=1 intD[1077]=1 intD[1096]=100
第 5 处不一致：
   oracle: 17 f=71(8,...) ret=1 ... intD[1316]=27 ...
   emu   : 17 f=71(8,...) ret=1 ... intD[1316]=6  ...     ← 写了一半、值还不对
```

读法：

* `emu` 的返回值 `0x10401000 / 0x10401100 / 0x10401200 …`（每次 +0x100）是**上一次调用残留的指针**，
  不是 `func 71` 的正常出口值（原生恒为 `1`）→ 说明 emu 在 `func 71` 的某处**提前 return / 走错分支**。
* `71(0)` 已经对了（`inc/dec` 那条修完），坏的是**实体 1 起**的类型处理路径（idx8）。
* 已定位过的一处：`WRITE intD[1030]=11 @10005fac`，而 `0x10005FA9` 是 `movl %edx, 0x78(%eax)`，
  按原生应当在 **idx8 的 case 0**、而不是 case 2 body。
  idx8 的 switch：`10005ba0: mov 0x8(%ecx),%eax; mov 0x10(%eax),%eax; decl %eax; cmpl $0xd; ja default;
  jmpl *0x10005bfc(,%eax,4)`，case-0 → `0x10005F30`、case-1 → `0x10005F40`。

### 下一步（顺序，别跳）

1. **收 `func 71` 的 idx8 分派**：在 idx8 入口把「实体下标 + 记录里的模式号」打出来，
   看每次派发的 `(实体, mode)` 序列，找出 mode 是在哪一步变成 2 的；
   同族线索：`incl 0x7c(%esi)`（field 31）被写成了 field 30，怀疑是**同一处字段偏移 / 宽度**问题。
2. **把 `emu.c` 编进 `app/src/main/cpp`**，`LittleBustersPT00DLL::CallDLL()` 转调它
   （DLL 仍由用户游戏数据提供，走 SAF / 应用目录读取）。
3. **然后才真机实测**：验证小游戏行为是否恢复正常，以及一直卡着的「TIME」是否松动。

### 7.1 第二批修复后的现状（2026-10-05 续）

上面的表与原始输出是**第一批修复后**的快照；第二批（§6.1）之后：

| 夹具 | 结果 |
| --- | --- |
| `tools/pt00_oracle/calls_sample.txt`（7 次） | **7/7 逐位一致** ✓（不变） |
| `build/pt00-fixtures/calls_long.txt`（280 次，带 `--seed`） | **280 次调用全部执行完、不再出现未实现指令**；差异 **252 → 42 处**，且全是**值级**差异 |

剩余差异全部集中在**实体类型 2/4/8/9/11/15** 的 AI/随机字段
（`record+4/+5/+28/+33`、全局 `intD[20]/[350]`），其中**首处差异 = 第 11 次调用 `71(2)`**：

```
第 1 处不一致：
   oracle: 11 f=71(2,0,0,0) ret=1 intD[20]=3 intD[350]=15 intD[1096]=100 intD[1098]=1 intD[1103]=1 intD[1105]=1
   emu   : 11 f=71(2,0,0,0) ret=1 intD[1076]=1 intD[1077]=1 intD[1096]=100 intD[1098]=1 intD[1100]=1 intD[1103]=1
```

读法：原生在类型 2 里走了**「选角色」分支**（写 `intD[350]=` rand 派生值、`intD[20]=3`、`record+33`），
我们走了另一边（写 `record+4/+5`）→ 分支条件在**类型 2 的 idx6/7/8** 里，这是下一步唯一的收敛点。

### 7.2 两个已知的近似（别当成已经搞定）

* **emu 不跑 PE 入口（DllMain / CRT 初始化）**：实测跑进去会在 CRT 自带堆初始化里**死循环**
  （平坦 guest 空间没有缺页语义，它的扫描永远找不到终止条件）。所以 CRT 的 ptd/TLS 目前是近似实现。
* **对照时两侧都把 `ptd+0x14`（rand 起点）钉成 1**：`--seed` 只对齐 intD/intF；rand 的种子来自
  `func_load` 开头的 `srand(time(&t))`，而 `time()` 由被桩掉的 `GetSystemTime/GetLocalTime` 推出来——
  两边未必同值。钉死后差异 **52 → 42**，**证明种子来源确实是分歧源之一**。

### 7.3 状态级对照（新判据）与「oracle 自身不可复现」

`compare.py` 比的是「这一帧改了哪些槽」；但 oracle 与 emu 可能把**同一个值写在不同的帧**里 ——
实测第 11 帧（`71(2)`）两边逐槽差异行不同，**整片状态却逐位一致**。所以判据应该是
「第 N 帧结束时 `intD`+`intF` 是否逐位一致」，工具是 `tools/pt00_oracle/state_walk.py`
（配套 `emu --dump <file>`，与 oracle 的 `out.bin` 同格式：`intD[2000]+intF[2000]` 小端 int32）。

跑前 13 帧的结果：

| 帧 | 结论 |
| --- | --- |
| 1–11（含 `71(0)/(1)/(2)`） | **状态逐位一致** |
| 12（`71(3)`） | 第一处状态差异，只有两个槽 |

```
intD[1112]（实体 3 的 mode = record+0x10）  oracle=1096040772 (=0x41544144)  emu=10
intD[1136]（record+0x70）                  oracle=0                        emu=1
```

**关键：oracle 自己不可复现。** 同一输入、同一条命令，跨会话跑出过 `intD[1112]=10` 与 `=1096040772`
两种结果（同一会话内连跑 6 次却是稳定的）→ 这条路径**读了未初始化内存**：我们的 guest 堆是 `calloc` 全 0，
原生堆里是残留数据（`0x41544144` 恰好是 ASCII `ATAD`，也像 2004-09-24 的 time_t）。

含义与对策：

1. **这类槽位不能当判据** —— 追它们是在追原生的未初始化内存，不是我们的 bug；
2. 夹具是合成的（只填了少数 intD、其余全 0），真实游戏里这些字段由脚本填，
   所以「0 vs 残留」很可能是**夹具假象**；
3. 后续要么给 oracle 补「把可疑输入显式清零」，要么改成「多会话复测 + 只比稳定槽位」。

## 8. 相关但不同的问题（别混淆）

| 项 | 状态 |
| --- | --- |
| 小游戏卡在 `TIME`（暂停） | 复盘见 `docs/LB-MINIGAME-RETRO.md`（含 5 条被证伪的假设）；本轮由兼容层接管 |
| 主要人物未渲染（实体渲染不全） | 独立问题，未解 |
| `Sel` objbtn 组（21/22/23/30/32） | **已补**（`66d58af`，落在平台层 `app/src/main/cpp/native-bridge.cpp`，不动上游语义） |
| 对话栏渲染、影片音画不同步 | 见 `dev-log/OPEN-ISSUES.jsonl` |
| 存档标题空白 / LBEX LOAD 列表为空 | 已修（`3fc96d5` 等），见 `docs/PROGRESS.md` |

## 9. 本机路径速查（工具链）

| 用途 | 路径 |
| --- | --- |
| 原版 DLL（真值） | `G:\Little Busters! EX\Little Busters! EX\PT00.dll`（两端文件相同） |
| VS 2022 | `E:\VISUAL STUDIO`（`vswhere` 可定位；`build.bat` 里按需改 `vcvarsall`） |
| IDA Pro 8.3 | `E:\BaiduNetdiskDownload\IDA\IDA_Pro_v8.3_Portable`（`idat.exe`） |
| x64dbg | `E:\DBG\snapshot_2026-05-27_12-11\release\{x32,x64}`（**32 位 DLL 用 x32dbg**） |
| NDK 自带 objdump | `E:\DEV\AndroidSdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe` |
| `codeex`（Layer 2 生成器） | `C:\Users\tooke\.codeex-build\bin\CodeEX.Cli\Debug\net10.0\codeex.exe`（不在 PATH；Release 较旧、无 `tables`） |

> 读中文文件用 `cmd /c "chcp 65001 >nul & type <文件>"`；PowerShell 管道会乱码。
> Gradle 构建、git 写操作、adb 都需要提权（沙箱把 `~/.gradle` 与 `.git` 设为只读）。
