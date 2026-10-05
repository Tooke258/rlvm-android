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

## Layer 2 · 数据提取（声明式清单 + 生成器，已落地）

**2026-10-05 已落地**：`tools/dump_tables.py` 升级为「声明式清单 + 生成器」，一条命令可复现提取：

```powershell
python tools/dump_tables.py --manifest tools/pt00_tables.json --dll <PT00.dll> --out generated/pt00_tables.inc
```

* 清单 `tools/pt00_tables.json` = 单一事实来源：每张表 地址 / 条目数（固定值 或 `count_from` 全局 dword VA，生成时先读该地址的 uint32）/ C++ 数组名 / 备注。新增表只加一行。
* 产物 `generated/pt00_tables.inc`：`const uint32_t pt00_anim_frames_*[]` + `constexpr size_t pt00_anim_count_*`，命名对齐 Layer 1 约定（帧增量表 / 条目数 / 序号表）。
* 保留 `pt00_prefix_sum_table` 语义：`.data` 里是**帧增量**，运行期前缀和才是阈值（注释里显式标注）。
* 生成器对每张表显式回报（名称 / 地址 / 条目数来源 / 实读条目数 / 字节数），任一步失败即报错并 exit≠0（不静默跳过）。
* 旧交互模式（`--range` / 裸地址）保留。

与外部 CLI 的接口约定（若 CodeEX 的 CLI 想对齐）：输出「地址 → 整数数组」的文本/JSON，
条目数从另一个全局地址读取，并保留 `pt00_prefix_sum_table` 的语义。

## Layer 3 · Oracle 与差分回归（待做）

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
