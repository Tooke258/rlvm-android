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

设备侧 `.rlvm/KEY_リトルバスターズ！ＥＸ/`：

```
global.sav.gz            当前档
global.sav.gz.bak        真档备份（216/793/128/103/46）★
global.sav.gz.kidoku     引擎写过的（含全位 kidoku，1177→28621B 的那份）
global.sav.gz.kidoku-full / .r3 / .broken   各轮实验档
```
