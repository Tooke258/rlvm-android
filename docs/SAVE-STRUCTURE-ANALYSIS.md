# 存档结构系统分析（RLVM global ↔ 原生 RL ↔ 脚本）

> 2026-10-07。目的：把"完美解锁"这件事从试组合变成**有据可查的工程**。
> 相关：`docs/SYS1005-BALL-ROOTCAUSE.md`、`docs/MINIGAME-HIT-DIRECTION.md`、
> `tools/rlvm-global.py`、`tools/seen_entrypoints.py`。

## 0. 一句话现状

RLVM 的进度**只在** `<app files>/.rlvm/KEY_リトルバスターズ！ＥＸ/global.sav.gz` 里；
游戏的收集度是**脚本按 `intZ` 算的**；而引擎会**重算**一部分 `intG` 计数。
**原生 RL 的 `SAVEDATA/*.sav` 与 RLVM 完全不通用**（实测：整套拷过去仍是 3%）。

## 1. RLVM global.sav.gz 的确定布局

来源：`rlvm-release-0.14/src/machine/serialization_global.cc:80-107` +
本机实测（解压后 19,610 字节 → token 6837 个；未删改时）。

序列化顺序（`boost::archive::text_oarchive`，zlib 压缩）：

```
22 serialization::archive 20      ← boost 归档头
CURRENT_GLOBAL_VERSION
GlobalMemory {
    int         intG[2000]        ← **token[6] 起**：2000 个十进制数
    int         intZ[2000]        ← **token[2007] 起**
    string      strM[2000]        ← **token[4008] 起**：全空字符串 ⇒ 看起来就是 2000 个 0
    string      global_names[SIZE_OF_NAME_BANK]
    map<int, dynamic_bitset<>> kidoku_data   ← ★ 既读/看过，真正的"场景已读"
}
SystemGlobals / GraphicsSystemGlobals / EventSystemGlobals /
TextSystemGlobals / SoundSystemGlobals / cursor_mono / reduce_distortion / sound_quality
```

实测对照（真档）：

| 区域 | 实测 |
| --- | --- |
| intG | 216 项非 0；收集区 `[1000..1042]`、计数 `[1009]=128 [1021]=103 [1036]=46` |
| intZ | 793 项非 0（含 `[391]=542711623` 这种位域） |
| strM | 全 0（2000 个空串）——**不是"第三张标志表"**（我一开始就是这么误判的） |
| kidoku_data | 真档里很小；`kidoku_unlock_all` 打满 10000×64 位后 token 数 6837 → 36825（档 1177 B → 28.6 KB）✓ |

## 2. "看过/解锁"的判定（源码）

`src/machine/memory.cc:199-213`：

```cpp
bool HasBeenRead(int scenario, int kidoku) {
  auto it = global_->kidoku_data.find(scenario);
  if (it != end && kidoku < it->second.size()) return it->second.test(kidoku);
  return false;                       // 条目不在 / 位数不够 ⇒ 一律 false
}
void RecordKidoku(int scenario, int kidoku) { /* resize 到 kidoku+1，再 set */ }
```

脚本侧的写入点：`libreallive/bytecode.cc:299` —— 走到 `{- Kidoku N -}` 标记时
`machine.SetKidokuMarker(value_)`，真 id = `kidoku_table[N] - 1000000`（entrypoint 序号）。

**⇒ 结论：`kidoku` 的空间很小（每场景不过几十位），"全打满"是合法的，
而且这条路我们已经打通**（用 `kidoku_unlock_all` 让引擎自己写，日志有证：
`kidoku_unlock_all: marked 640000 bits and flushed global`）。

## 3. 收集度是脚本按 `intZ` 算的

按场景统计 `intZ[` 引用数（`build/rlvm-scenes.txt`）：

| 场景 | `intZ[` 引用 | 说明 |
| --- | --- | --- |
| **SEEN9001** | **669** | ★ 收集/鉴赏主场景 |
| SEEN9024 | 312 | 收集相关 |
| SEEN3000 / SEEN9000 / SEEN9002 | 138 / 131 / 117 | 同为 90xx 鉴赏块与 3000 段 |
| SEEN9516 / SEEN3600 / SEEN521 | 111 / 81 / 65 | |

而 `intG` 的门槛在另一组场景：

| 场景 | 读的 `intG` |
| --- | --- |
| SEEN9012 | `[1..6,15,16,25,26,1030,1031,1032,1033,1034,1038]` |
| SEEN9020 | `[1,38,39,43,1040,1041,1042]` |
| SEEN9023 | `[1040,1150,1360,1361]` |

**注意**：`SEEN9001` 里对 `intZ` 的访问是**计算下标**（`intZ[基址 + i]` 形式），
所以静态抓"字面下标"会得到 0 —— 这也是为什么"整表填 1"只能解锁一部分。

## 4. 实测行为表（哪些能固化、哪些被引擎重算）

| 我改的东西 | 引擎重写后 | 结论 |
| --- | --- | --- |
| `intG[1..6,25,26,38,39,43,1360]=1` | **保留** | 能固化（且确实解锁过主线后期 scene） |
| `intG[1000..1045]` 0→1 | 保留，但 `[1033]/[1034]` 被重算回 0 | 部分是派生位 |
| `intG[1036]` 写 200 | **被重算回 46** | 派生计数 |
| `intG[1021]` 写 200 | 引擎自己 +1（游玩中自增） | 引擎维护的活计数 |
| `intZ` 填 `1` | 保留 | 只解锁一部分（正确值就是 1 的条目） |
| `intZ` 填 `-1` | **引擎清零** | 值域非法 ⇒ 崩溃 |
| `strM` 第三数组（全 0） | —— | 它是 `strM` 空串，不是标志表（别再动） |
| `kidoku_data` 全位（引擎写） | **保留且暴涨** | 合法，但**不影响那个 3%** |
| 原生 `SAVEDATA/*.sav`（PC 整套） | 无效 | RLVM 不读游戏目录那套 |

## 5. 已证伪的做法（别再走）

1. 只改 `intG` 收集位/计数 ⇒ 不动 3%；
2. 把 `strM`（被我误认的"第三张表"）填 1 或 -1 ⇒ 无效果 / 崩溃；
3. 把 `intZ` 填 `-1` ⇒ 清零；
4. 只填 `kidoku_data`（哪怕全位）⇒ 不动 3%；
5. 搬原生 `SAVEDATA`（`read.sav`/`LBEX_SC.sav`/`save999.sav`）⇒ RLVM 不读；
6. 手写 boost 序列化去构造 `kidoku_data` ⇒ 不如调 `Memory::RecordKidoku` 让引擎写。

## 6. 还缺的两块拼图（下一步）

1. **`SEEN9001` 的 `intZ` 循环**：基址、上界、每个条目**期望的取值**
   （`== 1`？`== 2`？还是位域值）。读出来就能按区间精确填，而不是整表拍平。
2. **派生计数的来源**：`intG[1033]/[1034]/[1036]` 每次启动被重算、`[1021]` 随游玩自增 ⇒
   引擎一定有"从某处统计"的逻辑。它在 RLVM 源码里应该能找到（搜 `1033/1036/1021`
   这类魔数，或搜 `intG` 写入点），也可能在脚本里（`SEEN3000/3600` 段读 `intF` 非常多）。

## 7. 工具与验证回路（关键：秒级验证器）

| 工具 | 用途 |
| --- | --- |
| `tools/rlvm-global.py` | `dump / set / preset / fill`（`--array 0/1/2` 选 intG/intZ/strM），无损改归档 |
| diag `kidoku_unlock_all=1` | 让**引擎自己**把全场景 kidoku 打满并落盘（日志留证）—— 以后要给 global 注入任何状态，优先走引擎 API |
| **`LBEX完成度统计器.exe`** | **秒级验证器**：指向任一份 `SAVEDATA` 目录即报总完成度与各线百分比。⇒ 迭代从"装包+试玩"变成"改完看数字" |
| `tools/seen_entrypoints.py` | `#entrypoint N` 是 kidoku 表下标，真 id = `kidoku_table[N]-1000000` |

## 8. 备份现状（都可回滚）

## 9. 【新】3% 的算式与数据源（本轮定位）

## 10. 纠正：两个指标要分开；收集 = intZ 记录 + intF 基址（必须成对）

### 10.1 两个不同的指标（此前混在一起是主要弯路）

| 指标 | 数据在哪 | 离线读数器 | 现状 |
| --- | --- | --- | --- |
| **文本已读率** | 原生 `SAVEDATA/read.sav` ↔ RLVM `global.sav.gz` 的 **`kidoku_data`** | **`LBEX完成度统计器.exe`**（PC 报 80.3%） | 位图段已定位（`read.sav` ≈ offset 40150 之后）；RLVM 侧 `kidoku_unlock_all` 已能写满 ✓ |
| **收集率（CG/音乐/场景）** | `intZ` 记录（global）+ `intF` 基址（local/槽档） | **仅游戏内相册界面**（统计器不管这一项） | 未解决；游戏里那个 3% 是这一项 |

* 统计器 = **文本已读率**的读数器（用户实测确认）⇒ 用它标定 `read.sav` 是对的，
  但**不能**用它验证 CG/音乐解锁。
* 往 `read.sav` 的零段填 `0xFF` ⇒ 统计器直接 0%（值域非法/越界），
  所以 `read.sav` 的位图段只能小范围、按标定值改，且**必须留 base 对照**。

### 10.2 收集/相册 = 两组数据，缺一不可

来自 `SEEN9001` 的写法 + `machine/memory.h`：

```
intZ[intF[1100] + 12] = 0        ← 记录本体在 intZ（GlobalMemory，存 global.sav.gz）
intZ[intF[1101] + 27] = …          记录字段偏移：+12..+24、+26..+88
                                  基址在 intF（LocalMemory，存槽档 saveNNN.sav.gz）
```

⇒ **只改 global 无效**（槽档里的 `intF` 基址没跟着变 ⇒ 记录不被引用）；
⇒ **把 `intZ` 填 `-1`/`1` 也无效**（记录值非法 ⇒ 崩或只显示一部分）。

### 10.3 槽档里的局部内存尺寸（手机 RLVM 实测）

`save000.sav.gz` 解压后是 **6 张 2000 长的 bank**：
非 0 数依次为 `14 / 0 / 3 / 301 / 1 / 331` ⇒ 活跃的是第 4、6 张
（`intD` 与 `intF` 量级相符）。

### 10.4 格式不通用（关键前提，别再混）

* 手机游戏目录里那份 **2016 年的 `SAVEDATA\*` 是原生 RL 格式**（随包附带），
  **不是** RLVM 存档；
* RLVM 的存档只在 `<app files>/.rlvm/KEY_リトルバスターズ！ＥＸ/`
  （`global.sav.gz` + `saveNNN.sav.gz`），两者**格式不同、不通用**。
* ⇒ 可做的对照只有两种：
  1. **原生 ↔ 原生**（PC 的 `SAVEDATA\*` ↔ 手机的 2016 那份）：同格式，能读出编码；
  2. **原生 ↔ RLVM**：只能靠语义对应（已知 `read.sav` ↔ `kidoku_data`）。

### 10.5 下一步

1. 用**原生 ↔ 原生**对照（PC 更完整）读出**原生侧收集数据的编码**；
2. 找到它在 RLVM 侧的对应物（预期是 `intZ` 记录 + 槽档 `intF` 基址），
   按 `SEEN9001` 的字段偏移构造；
3. 验证只有游戏内相册（没有离线读数器），因此**每次改动都要同时覆盖两半**，
   并用 `.rlvm/` 里的档备份来回滚。

### 9.1 百分比就在这三个场景里算

`build/rlvm-scenes.txt` 里含 `* 100` / `/ 100` 最多的场景：
`SEEN9250`(27) / `SEEN9240`(26) / `SEEN9260`(22)，其中 `SEEN9240` 的完整算式是：

```
intL[1]  = intD[380 + intL[0]]
op<2:084:01006, 0>(271, 0 + intL[1], 0, intL[20+0], intL[20+1])   ← 取"条目 0"的两个数
…（条目 1..4 同）
intL[2]  = 300 / 240 / 190 / 150 / 120 / 100      ← 分母，按 intD[400+intL[0]] 选类别
intL[3]  = 500 / 380 / 280 / 200 / 140 / 100
intL[20+10] = intL[20+0]+intL[20+2]+intL[20+4]+intL[20+6]+intL[20+8]   ← 分子（已完成）
intL[20+11] = intL[20+1]+intL[20+3]+intL[20+5]+intL[20+7]+intL[20+9]
intL[20+12] = intL[20+10] * 100 / intL[2]        ← ★ 这就是"完成度 %"
intL[20+13] = intL[20+11] * 100 / intL[3]
```

### 9.2 分子来自「模块 84 = GRP 记录表」的查询

`op<2:084:01006, 0>(271, base, index, &a, &b)` 里的模块 84 在 RLVM 里就是
**`src/modules/module_grp.cc`（GRP/"记录表"模块）**：同文件注册了
`AddOpcode(1100, 0..3, "recCopy", …)`（`module_grp.cc:1370`），与 dump 里
`2:084:01100` 对上；`1000/1006` 是 `recGet` 一系。

⇒ **完成度 = Σ(记录表 271 中各条目的"已收集数") × 100 / 总数**，
记录由 `recCopy/recSet` 在游玩中更新。日志里**没有**任何
`Undefined: opcode<2:84:…>` ⇒ 该模块在手机上**是实现了的**（假设"未实现 op"已排除）。

### 9.3 由此得到的强推测：`intZ` 就是 GRP 记录的后备存储

三条证据一致：

1. `intZ` 里的值形如 `[391] = 542711623`（打包记录值，不是 0/1 标志）；
2. 把 `intZ` 填 `-1` ⇒ 整个收集崩掉清零；填 `1` ⇒ 只"解锁一部分"（恰好撞上合法值）；
3. 而改 `intG` / `kidoku` / 原生 `SAVEDATA` ⇒ 3% **完全不动**（它们不是记录）。

### 9.4 下一步（纯源码，有界）

1. 读 `module_grp.cc` 的 `recGet / recCopy` 实现 ⇒ 弄清**记录落在哪块内存**
   （`intZ`？`strM`？还是别的 Memory location）以及 `base/index` 的寻址；
2. 找记录表 **271** 的布局（哪些字段是"已收集数/总数"，`intD[380+n]` 是什么表）；
3. 按记录格式构造"全收集"，用 **`LBEX完成度统计器` 秒级验证**
   （它显然与 §9.1 同口径）。

设备侧 `.rlvm/KEY_リトルバスターズ！ＥＸ/`：

```
global.sav.gz            当前档
global.sav.gz.bak        真档备份（216/793/128/103/46）★
global.sav.gz.kidoku     引擎写过的（含全位 kidoku，1177→28621B 的那份）
global.sav.gz.kidoku-full / .r3 / .broken   各轮实验档
```

---

## 11. 相册三栏的真源（2026-10-07，真机验证通过）

§9.2 / §10.2 的"模块 84 = GRP 记录表"是**误读**：`op<2:084:01006,0>` 的
type2/module84 是 `ChildObjFgGetters`（opcode 1006 = `objGetAdjust`），
而 `module_grp.cc` 里的 `recCopy` 是**矩形拷贝（blit）**，不是记录表。

相册（`SEEN9110` 分页 → `SEEN9515/9516/9517`）三栏各有自己的真源：

| 栏 | 场景 | 判定 | 真源 |
| --- | --- | --- | --- |
| Gallery(CG) | SEEN9515 | `Sys 1504`(cgStatus) 逐条问 g00 文件名 | `CGMTable::cgm_data_`（`dat/mode.cgm` 的 文件名→flag） |
| Scene | SEEN9517 | `intZ[390*32 + 0..15] == 1`（14 条：`O_SCM_FGKM13`…`FGSY13b`） | intZ 位域 word 390 |
| Music | SEEN9516 | `intZ[391*32 + 0..55]`（56 条） | intZ 位域 word 391/392 |

* `cgm_data_` 属于 `GraphicsSystemGlobals`，随 `global.sav.gz` 落盘
  （`serialization_global.cc:100`），`Sys 1500-1504` 是它给脚本的接口。
* **顺序坑**：`CGMTable::SetViewed` 按"字"写 `intZ[flag]=1`（flag=0..409），
  而 Scene/Music 用的是"位"（`390*32+N` / `391*32+N`）——同一个 intZ 字
  被两种语义共用。所以必须先 cgm、后 collection，否则 `intZ[391]/[392]`
  会被 `SetViewed` 覆盖成 1，音乐栏只剩一半甚至全空。
* 手改 boost 文本归档不可靠（§9/§10 用的就是那条路）。正确姿势是让**引擎自己写**：
  `Memory::RecordKidoku` / `CGMTable::MarkAllViewed` /
  `SetIntValue(IntMemRef(INTZ_LOCATION, 1, bit), 1)`，再 `Serialization::saveGlobalMemory`。

### 11.1 可用的 diag（`rlvm-diag.txt`，一次启动按 kidoku → cgm → collection 顺序跑）

```
cgm_unlock_all=1          # Gallery：CGMTable::MarkAllViewed（410 条）
collection_unlock_all=1   # Scene 位 390*32+0..15 + Music 位 391*32+0..55（72 位）
kidoku_unlock_all=1       # 全场景 kidoku（文本已读率）
cgm_dump=1                # 打印 mode.cgm 的 文件名=flag 清单
loop_detect=1             # 每 2 秒写一行"当前最热的 (场景,行号)"；不再新增=引擎停摆非空转
longop_log=1              # 长操作 push/pop（带类型名）；只有 push 无 pop = 卡在"等某动作"
```

真机日志样例（`rlvm-log.txt`）：

```
kidoku_unlock_all: marked 640000 bits and flushed global
cgm_mark_all: entries=410 intZ_set=410 out_of_range=0 cgm_data=410 flag_min=0 flag_max=409; global flushed
collection_unlock_all: set 72 bits (scene 390*32+0..15, music 391*32+0..55); global flushed
```

产物：`build/global-perfect-engine.sav.gz`（37224 token，三栏全解锁，全由引擎写出）。

### 11.2 已知未解：Scene 回想"点进去"无条件卡死

`SEEN9517` 选中条目后走 `op<0:001:00004,0>(intA[32])`（goto_case 跳表），
每个分支是一条 `farcall(<剧情场景>, 90/91/92)`：

```
entry 0  → farcall(1003, 90)     entry 6  → farcall(6003, 90)
entry 1  → farcall(3000, 92)     entry 7  → farcall(6003, 91)
entry 2  → farcall(4002, 90)     entry 8  → farcall(2603, 90)
entry 3  → farcall(4002, 91)     entry 9  → farcall(2603, 91)
entry 4  → farcall(4103, 90)     entry 10 → farcall(3600, 90)
entry 5  → farcall(5006, 90)     entry 11 → farcall(1203, 90)
```

真机表现：页面正常、条目能点，**一进回想起就卡死**；与存档位域无关（位是全亮的）。
注意 `SEEN1003` 只有 `#entrypoint 0/371/696/1206`，没有 90
⇒ farcall 的第二个参数**不是**"本场景的 entrypoint 序号"。
下一步：开 `op_trace` 抓一次回想入口，看这条通路卡在哪个循环。

#### 11.2.1 已经排除/已确认的

* ⚠️ **更正**：12 个 `farcall(<剧情场景>, 90x)` 的目标 entrypoint **是存在的**。
  我一开始拿 dump 里的 `#entrypoint N` 去比对，那是**kidoku 表的下标**，不是真实 id：
  `bytecode.cc:273` 里 `entrypoint_index_ = kidoku_table[value_] - 1000000`。
  用 `tools/seen_entrypoints.py "<游戏目录>/SEEN.TXT" 1003 3000 …` 离线核对结果：

  ```
  SEEN1003 索引 696  -> id 90
  SEEN3000 索引 4652/4788/6412 -> id 90/91/92
  SEEN4002 索引 83/234        -> id 90/91
  SEEN4103 索引 314           -> id 90
  SEEN5006 索引 624           -> id 90
  SEEN6003 索引 1845/384      -> id 90/91
  SEEN2603 索引 923/1283      -> id 90/91
  SEEN3600 索引 6834          -> id 90
  SEEN1203 索引 911           -> id 90（还有个 89）
  ```

  （`tools/seen_entrypoints.py` 的 docstring 早就写了这个坑，我这次是亲自踩了一遍。）
  所以"回想"在机制上**本该能跑**，卡点不在 entrypoint 解析。
* 逐指令 trace（`op_trace=farcall,rtl,gosub,goto`）**一条都没打印**：
  循环体里没有这些控制流指令。
* 全量 trace（`op_trace=*`）注意两件事：
  1. **trace 输出走 logcat（tag `rlvm-stderr`/`rlvm-stdout`），不进 `rlvm-log.txt`**；
     logcat 是环形缓冲，只能看最近约 6 万行 —— 早先"日志里 grep 不到 Undefined"
     就是这个原因导致的误判。
  2. 点条目后的 trace 尾部是：
     ```
     (SEEN9517)(Line 0244): goto_unless(100 <= intF[1350] …)   ← 条目被选中
     (SEEN9517)(Line 0264): gosub()
     (SEEN9517)(Line 0282): bgmFadeOut(1000)
     (SEEN9517)(Line 0283): gosub()
     (SEEN9517)(Line 0118): bgrLoadHaikei("?", 0)
     (SEEN9517)(Line 0121): objFreeAll()
     (SEEN9517)(Line 122):  Undefined: opcode<1:4:2402, 0>()   ← Sys 2402 未实现
     (SEEN9517)(Line 0125): ret()
     (SEEN9517)(Line 0284): bgmFadeOutEx()
     ```
     之后就没有新行了 ⇒ 要么停在 `bgmFadeOutEx()` 里（它是非阻塞的原子操作，
     不太可能），要么是**预算/logcat 环形缓冲在这里截断**了。所以不能用它下结论。
* 新工具：`loop_detect=1`（`tools/rlvm-diag.loop-detect.txt`）
  —— `RLModule::DispatchFunction` 里每派发一条指令都调
  `rlvm_android::NoteOpForLoopDetect(scene, line)`，平台侧保留最近 24 条
  (场景,行号)，尾部 8 条按周期 1..4 重复就写一行到**应用日志**（不走 logcat）：
  ```
  loop_detector: hot loop period=2 at (SEEN9517)(Line 0244)
  ```
  这是下一次真机复现时要看的唯一一行。
* 另一个未实现的 opcode 记录在案：**`Sys 2402`（`op<1:4:2402,0>`）**，SEEN9515/9517
  都在进页时调用它；它未实现这件事本身在 Gallery 上是无害的（Gallery 正常），
  但回想通路上可能不是。
  Sys 24xx/15xx/25xx 这些高位 opcode 属于**汉化补丁自己的引擎**（游戏目录里的
  `LBEX_CHS.exe` / `lbex_sc.dll`），RLVM 要补齐只能去反这两个文件里的分发实现。

#### 11.2.2 卡点收窄（结合 trace）

点击条目后 trace 的**最后一条派发指令**是：

```
(SEEN9517)(Line 0284): bgmFadeOutEx()
```

而回想真正起跳的 `goto_case(intA[32]) → farcall(1003, 90)` 那段在它**之后**，
所以卡点在"起跳前的收尾动作"上，两种可能：

1. 真的阻塞在 `bgmFadeOutEx()` 内（它自己只是写几个原子量，嫌疑不大；但音频
   引擎侧若在锁上打转就会这样）；
2. **logcat 环形缓冲把后面的行截断了**（trace 走 logcat，只留最近约 6 万行）。

`loop_detect=1` 正好区分这两种：有 `loop_detector: hot loop` 行 ⇒ 是循环；
一行都没有 ⇒ 是阻塞在某个调用里，下一步就是把 `Sys 2402` / `bgmFadeOutEx`
这两个候选逐个补上或加日志。

#### 11.2.3 三个页面入口段逐行对照（`build/tmp_entry_cmp.py`，9515 vs 9517）

三个页面是同一套模板，入口段调用的 Sys 集合却不一样：

| 页面 | 入口段调用的 Sys |
| --- | --- |
| SEEN9515 Gallery（正常） | `105` → `1521` → `2402` → `1520` |
| SEEN9516 Music（正常） | `105` → `2402` → `105` |
| SEEN9517 Scene（卡死） | `105` → `2402` → `106`(bgmFadeOutEx) → **`1213(0)+457(0)`** → **`1213(1)+457(1)`** → **`457(2)`** → `1211 v1` → `goto_case` → `farcall(1003,90)` |

这些 opcode 在 RLVM 里的实现状态（`tools/sys_opcode_coverage.py` + 直接扫注册表）：

| opcode | 用次数 | RLVM |
| --- | --- | --- |
| `1213` DisableSyscom | 52 | ✅ 已实现（`module_sys_syscom.cc:74`） |
| `457` | 56 | ❌ 未实现 |
| `2402` | 39 | ❌ 未实现 |
| `2502` | 39 | ❌ 未实现 |
| `1520` / `1521` | — | ❌ 未实现 |
| `366` / `801` | 61 / 57 | ❌ 未实现 |

⇒ **Scene 入口这一整套"禁用系统菜单 → 清按钮点击状态 → 重新允许系统菜单 → 跳进回想"里，
有一半在我们这儿是静默跳过的**。`1213(n)` 与 `457(n)` 在全库永远成对出现（52/56 次）、
且总是紧跟 `1212`(HideSyscom)，因此 `457(n)` 极可能是"清掉第 n 个系统按钮的点击/按下状态"
——正好就是"换入口时把输入状态归零"这件入口端的活。

下一步（两条并行）：

1. **离线**：反 `LBEX_CHS.exe` / `REALLIVE.EXE` 里 `Sys 457/2402/2502/1520/1521` 的实现，
   把语义补进 RLVM 的 Sys 模块（挂 no-op 桩没有意义——现在的"跳过"本来就是 no-op）。
2. **真机**：`loop_detect=1` 点一次 Scene 条目，看 hot loop 落在 SEEN9517 入口段
   （坐实入口死等）还是落在重播场景内部（说明入口已经过了）。

#### 11.2.4 REALLIVE.EXE 结构探查（2026-10-07，离线，未出结论）

* 段布局：`.text 0x401000..0x629c52` / `.rdata 0x62a000`（14 KB）/ `.data 0x62e000`（file 内仅 0x25f40）
  / `.rsrc`。**没有 `.reloc`** ⇒ 指针是绝对 VA，理论上能靠扫描找表。
* 用 `build/tmp_pe_struct_tables.py`（步长 8/12/16/20/24/32）扫下来，**没有**
  "opcode → handler 的函数指针数组"；0x62a538 那一大段是字符串/指针混排的资源表，
  不是 Sys 分发表。（脚本初版把 RVA 当 VA 比，已修。）
* `CGTABLE_FILENAME` 字符串在 VA `0x631c3d`，但整个 `.text` 里**既没有它的绝对 VA 引用、
  也没有 RVA 引用** ⇒ 引擎要么用"基址+偏移"的间接寻址，要么 `.text` 有轻量混淆。
  **结论：靠裸字节 xref 追不动，得上 IDA**（游戏目录里已经有 `REALLIVE.EXE.i64`，33 MB，
  用户之前建过库）。
* 重要旁证（来自 `dev-log/REALLIVE-ENGINE-RECON.jsonl`）：**PC 汉化版读的是 `seen_sc.bin`**
  （头部 `77 16 00 00` 的自定义容器），而 RLVM 读的是 RealLive 归档 `SEEN.TXT`
  ⇒ **两边跑的不是同一份脚本**，不能拿 PC 的行为当"同脚本等价"的判据。

### 11.3 ✅ 结案：Scene 回想卡死的真正原因（2026-10-07，已真机验证）

**根因不在脚本、不在存档、也不在缺 opcode，而在我们自己的音频层。**

链路：

```
SEEN9517 点击条目
  → 入口收尾: op<1:020:00106,1> = bgmFadeOutEx()          （脚本第 284 行）
  → RLVM module_bgm.cc: bgmFadeOutEx = "带等待"的版本
      WaitLongOperation + BreakOnEvent(BgmWait)
      BgmWait 的判据: machine.system().sound().BgmStatus() == 0   ← 等 BGM 真的停
  → 我们的 AndroidSoundSystem::BgmFadeOut 只做了【音量】淡到 0
      （AudioEngine::FadeVolume(ch, 0, ms)），**通道一直算"在播"** ⇒ BgmStatus() 永远是 1
  → 那个 WaitLongOperation 永远不返回 ⇒ 引擎不再派发任何指令 ⇒ 表现成"点进去必卡死"
```

还有一处放大器：音频回调里"淡出完成 → `playing=false`"的逻辑写在读环形缓冲**之后**，
且 `got == 0` 时直接 `continue` ⇒ 缓冲一空，停通道的请求就永远不会被处理。

**修复（只动我们自己的 Android 音频层）**：

1. `AudioEngine::FadeOutAndStop(channel, ms)`：音量线性淡出到 0，**并在到点时真正停通道**
   （对齐 SDL 端 `Mix_FadeOutMusic` 的语义）；`AndroidSoundSystem::BgmFadeOut` 改用它。
2. 音频回调把"到点停"的判断挪到读环形缓冲**之前**；并且 `got == 0` 时若已请求停播就立即停。

**真机证据（`rlvm-log.txt`）**：修复前最后一行是

```
longop push: 17WaitLongOperation @(SEEN9517)(Line 284)     ← 之后再无任何行（引擎停摆）
```

修复后同一位置变成

```
longop push: 17WaitLongOperation @(SEEN9517)(Line 284)
longop pop:  17WaitLongOperation                          ← ★ 正常结束
loop_probe: 2s top=(SEEN9030)(Line 108) ; 2nd=(SEEN9517)(Line 366)
loop_probe: 2s top=(SEEN2801)(Line 174)                   ← 进到回想正文场景
```

屏幕上确实出现了回想正文（SEEN2801 的对话）。同类隐患：任何"等 BGM 停"
（`bgmFadeOutEx` / `BgmStop` 后 BgmStatus）在缓冲排空时都可能踩同一个坑，现在一并修掉了。

### 11.4 ✅ 「剧情选择枝被直接忽略」结案（2026-10-07，已真机验证）

症状：**有些 scene 开头会出现一个选项，我们这里直接跳过**（像是被自动选掉了）。
正常的显示方式是**屏幕上一列上下排列的选择框**。

两处真实缺陷：

1. **`Sel 13`（`op<0:2:13>`）在解析层就被拆坏了。**
   `libreallive/bytecode.cc` 的 `BuildBytecodeElement()` 只把 `Sel 1/2/3/16`
   当 `SelectElement` 解析，`Sel 13` 不在名单里 ⇒ 那段
   `{ 条件, "はい"\n"いいえ"\n }` 的结构被当普通指令流拆散
   （dump 里就表现为 `"{"` / `"はい"` / `"いいえ"` / `"}"` 各占一行），
   后面那条 `Sel 13` 又因为模块里没注册而 `Undefined` 跳过 ⇒ 选项等于不存在。
   日志实证：`(SEEN9517)(Line 371):  Undefined: opcode<0:2:13, 0>()`。

   修复：
   * `bytecode.cc` 把 `0x0002000D` 并进 SelectElement 那条 case；
   * `module_sel.cc` 注册 `AddOpcode(13, 0, "select_13", new Sel_select_s)`
     —— 用和 `Sel 3` 同一套 `ButtonSelectLongOperation`（#SELBTN 定位、
     选项一列上下排列），而不是画在文本窗里的 `NormalSelectLongOperation`。

2. **`AndroidTextWindow::AddSelectionItem()` 是空实现。**
   该接口服务于"文本窗内选择"（`Sel 1` / `NormalSelectLongOperation`）：
   选项文字既不显示、也不进 `selections_`。已按 SDL 后端同一套实现
   （`FontEngine` 光栅化 → `SelectionElement` → 插入点下移一行）。

补充事实：屏幕上一列上下排列的选择框由 `ButtonSelectLongOperation` 负责，
每个选项的图来自 **`TextSystem::RenderText(...)`**（`#SELBTN` 的 MOJISIZE /
BASEPOS / REPPOS / CENTERING 决定位置）。我们移植里 `RenderGlyphOnto` /
`GetCharWidth` 一直是实现了的，所以这条路本来就能画——之前的"选项图标不见了、
点击映射还在"正是"选项图有、但根本没进到这一步"的表象。

诊断（长期保留，量很小）：

```
[rendertext] size=184x28 font=26 text="沙耶さんの勝ち"
[selbtn] options=2 reppos=(24,56) #0 rect=(108,200,561x42) img=184x28 #1 rect=(132,256,561x42) img=106x28
```
