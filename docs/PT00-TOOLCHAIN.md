# PT00（LB 棒球小游戏）逆向与移植工具链

> 2026-10-05 起。分三层，每层尽量用已有或自研的小工具，不引大依赖。
> Layer 2 打算换成用户在并行迭代的 CLI 工具（见下），本文先记录 Layer 1 与 3 的约定。

## Layer 1 · 符号化（已完成）

**目标**：以后每次导出 `PT00.dll` 的 Hex-Rays 伪码都带可读名字，移植时不必做
「地址 ↔ 语义」的心算，review 时也能直接看懂。

| 文件 | 作用 |
| --- | --- |
| `tools/pt00_symbols.tsv` | **唯一事实来源**：地址 / 类型(func·data) / 名字 / 注释。新增符号就往里加一行 |
| `tools/ida_apply_symbols.py` | IDA 批处理脚本：按 TSV 改名 + 加注释，然后调用同目录的 `dump_dll.py` 导出带名字的伪码 |
| `tools/dump_dll.py` | （已有）导出 PE 信息 / 导入导出表 / 函数清单 / 逐函数反编译 |
| `tools/dump_vtables.py` | （已有）按 VA 解 vtable，用来确定实体类型差异 |
| `tools/dump_tables.py` | （已有，Layer 2 的过渡）按 VA 导出 `.data` / `.rdata` 的 dword |

重跑（把 DLL 复制到临时目录再跑，别让 IDA 往游戏目录写 `.id0/.idb`）：

```powershell
$tmp = "$env:TEMP\pt00-ida"
idat.exe -A -L"$tmp\ida-sym.log" -S"<repo>\tools\ida_apply_symbols.py <repo>\tools\pt00_symbols.tsv $tmp\PT00.sym.dump.txt" "$tmp\PT00.dll"
```

**故意不依赖 `.idb` 的持久化**：改名是幂等的，仓库里只留文本清单，不带任何二进制数据库，
所以任何时候都能从「原 DLL + 清单」复现出同一份带名字的伪码。

首次运行结果：`applied=142 missing=0`（清单里的 142 条全部命中）。

### 命名约定

| 前缀 | 含义 |
| --- | --- |
| `pt00_funcNN_*` | `reallive_dll_func_call` 分派表里的编号 `NN` 对应的处理函数 |
| `pt00_entNN_idxM_*` | 实体类型 `NN` 的 vtable 第 `M` 槽（`M=6/7/8` 是类型差异所在） |
| `pt00_entity_*` | vtable 前 6 槽：所有类型共用（构造 / 绑定 intD / func70 / func71 / idx4 / 模式设置） |
| `pt00_anim_ids_*` · `pt00_anim_frames_*` · `pt00_anim_count_*` | 动画表三元组：序号表 / 帧增量表 / 条目数 |
| `pt00_rt_frames_*` | 运行期表（`pt00_prefix_sum_table` 把帧增量表前缀和后的结果） |
| `pt00_*_state` · `pt00_*_block` | 全局状态块 |

## Layer 2 · 数据提取（`codeex tables`，已落地）

**2026-10-05 定稿**：以用户并行项目 CodeEX 的 `codeex tables` 作为 Layer 2 的生成器。

```powershell
codeex tables "<PT00.dll>" --manifest tools\pt00_tables.json --out generated\pt00_tables.inc
```

（本机 `codeex` 不在 PATH 上；最新构建在
`C:\Users\tooke\.codeex-build\bin\CodeEX.Cli\Debug\net10.0\codeex.exe`。）

* `tools/pt00_tables.json` = **单一事实来源**：每张表 地址 / 条目数（固定 `count` 或
  `count_from` 全局 dword VA）/ C++ 数组名 / 备注。新增表只加一段。
* `generated/pt00_tables.inc` = 生成物，头部带生成命令、`DO NOT EDIT`。
* 保留 `pt00_prefix_sum_table` 语义：`.data` 里是**帧增量**，运行期前缀和之后才是帧阈值；
  `.inc` 保持原始增量，前缀和由实现侧做。
* 每张表显式回报（名称 / 地址 / 条目数来源 / 实读条目数 / 字节数），任一步失败即报错 exit≠0。

### 已提取：类型 0 的 9 组动画表（18 张）

| 动画集合 | ids | frames | count |
| --- | --- | --- | --- |
| `mB` | 0x1001F050 | 0x1001F054 | 0x1001F058 = 1 |
| `mC` | 0x1001F05C | 0x1001F060 | 0x1001F064 = 1 |
| `launch2` | 0x1001F068 | 0x1001F06C | 0x1001F070 = 1 |
| `launch` | 0x1001F074 | 0x1001F090 | 0x1001F0AC = 7 |
| `m4` | 0x1001F0B0 | 0x1001F0EC | 0x1001F128 = 15 |
| `m7` | 0x1001F12C | 0x1001F148 | 0x1001F164 = 7 |
| `m8` | 0x1001F168 | 0x1001F17C | 0x1001F190 = 5 |
| `m9` | 0x1001F194 | 0x1001F1A8 | 0x1001F1BC = 5 |
| `mA` | 0x1001F1C0 | 0x1001F1D4 | 0x1001F1E8 = 5 |

**布局自校验**：九组都严格满足 `[ids(count 项)] [frames(count 项)] [count 全局]` 首尾相接，
例如 `launch` 的 ids 占 `0x1001F074..0x1001F08F`（28B）、frames 占 `0x1001F090..0x1001F0AB`（28B）、
count 全局在 `0x1001F0AC` —— 说明地址与条目数都取对了。

> 注意别把 `0x1001F040` 当成表头：`0x1001F040` 起的头几项是 `0x1001986B`（指向 `.text` 的指针）等
> 别处的数据，只有从上面这些**具体地址**起才是表。

### 与 `tools/dump_tables.py` 的关系

`dump_tables.py` 在 `b27b80d` 里也加了等价的 `--manifest` 模式（同一套 schema，两者可以喂同一份清单），
产物只差条目数常量的命名（`pt00_anim_count_*` vs `<name>Count`）。**建议以 `codeex tables` 为唯一生成器**，
`dump_tables.py` 退回「按 VA 抽查几个 dword / 小范围」的检查用途（`--range`、裸地址），
避免同一份数据有两套格式。

## Layer 3 · Oracle / 执行器 / 差分回归（已落地 2026-10-05）

**路线已从「手写重写 PT00」切换为「兼容层：直接跑原版 DLL」**（决策与理由见
`dev-log/PT00-EMU.jsonl` 的 `decision` 记录）。三个工具：

| 工具 | 作用 |
| --- | --- |
| `tools/pt00_oracle/oracle.c` | **真值**：原生 `LoadLibrary` 跑原版 DLL，逐调用输出 intD 改动；`--seed` 对齐起点 |
| `tools/pt00_emu/emu.c` | **执行器**：自研最小 x86-32（PE 装载 / 整数 / 内存栈 / x87 / 导入桩 / TIB 影子 / 栈断言 / `--trace-func N`），约 1300 行 C |
| `tools/pt00_oracle/compare.py` | 同序列同种子跑两边**逐位对照**，退出码可当门禁 |

当前状态：`calls_sample.txt` **7/7 逐位一致**；`calls_long.txt`（280 次）有 184 处差异，
集中在 `func 71`，**怀疑是 `time()`→`srand()` 的非确定性**（下一步对齐时间源）。

规模已量死：可达代码 **4897 条指令 / 82 种助记符，无 SSE2**（详见同一份日志的 `static_analysis`）。

> 构建注意：Windows 侧用 VS 时**必须 `cl /utf-8`**——MSVC 默认按本地代码页读源文件，
> 中文注释会被错解、把后面的代码吞进注释里，表现为莫名其妙的语法错误。

### 下面这段是切换前的设计记录（保留）

**为什么**：把「手感一致」从肉眼判断变成可测数字；抄错一个系数立刻暴露。

**怎么做**（不需要任何商业素材入库）：

1. `tools/pt00_oracle/`：32 位 Windows harness（VS 编译）。`PT00.dll` 只靠
   `*(ctx + 0x14)` 拿 intD 基址、其余只用 CRT，所以造一个假 ctx 就够：
   `reallive_dll_func_load(ctx, 0)` → 反复 `reallive_dll_func_call(func, a1..a4)`，
   每步前后各导一份 intD 快照。
2. `tools/pt00_diff.py`：把设备上真实的调用序列（日志里的 `[pt00] #N f=.. a=(..)`）
   连同每步的 intD 快照喂回 oracle，与我们的实现逐项比对。

**边界**：oracle 只做验证，不进 APK、不进仓库；harness 脚本进仓库，验证素材放
`local-data/`（该目录不进仓库，见 `local-data/LOCAL-PATHS.md`）。

**排期**：先把「能跑」解决（起球链 + 类型 0 的 idx7/idx8），再上 oracle 解决「跑对」。

## 明确不采用

* Ghidra / RetDec 自动出 C 直接进项目：质量不可控，且会把「哪段是我们写的」搅浑，
  对 GPL 审查不友好。（用 Ghidra 交叉验证某一段可疑伪码可以。）
