# 渲染帧率 / 动画台阶问题 — 排查报告

> 状态：**已解决（2026-10-04）**——根因是 Android 后端的**计时基点**，已修复并真机验证。
> 本文件保留完整排查过程与数据；第 3 节「`force_wait` 是根因」的结论**已被推翻**，
> 见下面第 0 节。正式决策记录见 `docs/DECISIONS.md` 的 D-023。
>
> 记录时间：2026-10-04。设备：Redmi K40 游戏版（天玑 1200 / Android 12 /
> 120Hz）。涉及游戏：Little Busters! EX（手机端为日语原版）、Kud Wafter。

---

## 0. 结论：根因与修复（2026-10-04 定案）

**症状**：开屏淡变只有 8 级台阶（每级 alpha 跳 32/255），且**所有依赖帧计数器的
图层叠加动画**都同样粗糙；而渲染/合帧/GL 一切正常。

**根因**：`app/src/main/cpp/android/android_system.cpp` 的 `NowMillis()` 返回的是
`steady_clock` 的**绝对**毫秒（自开机起算，量级 10⁹），而 RLVM 的帧计数器把它存进
**`float`**（`systems/base/frame_counter.h`：`float time_at_last_check_`）。
单精度只有 24 位有效位，在 `[2³⁰, 2³¹)` 区间的 ULP 恰好是 **128ms**：

```
ms_elapsed 只能是 0 或 128  →  num_ticks = 128 / 0.1 = 1280（= 全量程 10000 的 12.8%）
→ 计数器每秒只推进 1000/128 ≈ 8 次  →  alpha 每级 32  →  正好 8 级台阶
```

PC 原生没有这个问题：`SDL_GetTicks()` 返回「自 `SDL_Init` 起的毫秒数」（10⁵~10⁶），
float 在该量级的分辨率是微秒级。**所以这不是 RLVM 的语义问题，而是后端把"相对 tick"
当成了"绝对时钟"。** 附带现象：ULP 随数值量级变化，所以严重程度还**随设备开机时长**
变化（开机 <12.4 天时 ULP=64ms ≈ 15 级，12.4–24.8 天时 128ms = 8 级）。

**定位手段（可复用）**：`rlvm-diag.txt` 的 `loop_probe=1` 每秒给出
「轮数 / 字节码指令 / long op 步进 / 栈顶 long op 类型 / 最热场景与行」；
配合在 `ReadFrames` 与 `FrameCounter::ReadNormalFrameWithChangeInterval` 里临时插
三重计数（读次数 / 值推进次数 / 非活跃读次数），一次运行即可定案。

**修复后实测**（Redmi K40 游戏版 / LBEX）：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| `FrameCounter` 值推进次数 | 5 / 8 次每秒 | 451 / 522 / 588 次每秒 |
| `content changed` | 8.3 /s | 92 ~ 123 /s |
| alpha 每级步长 | 32 / 255 | 3 ~ 4 / 255 |
| 合帧 vs 内容变化 | 124.7/s 合帧、8.3/s 有变化（94% 重复） | 121.7/s = 121.7/s（1:1） |

用户真机确认：**已平滑**。

**遗留**：`exec_ignore_force_wait`（第 5.3 节那条改动）**并非根因**，目前仍默认 true；
是否保留建议做一次 A/B（见第 6.3 节）。

---

## 1. 用户可见症状

游戏开屏的实时渲染动画（"この作品はフィクションです…" 警告淡入淡出、Key logo
淡入）呈现**明显的阶梯感 / 抽帧感**，用用户的原话：

> "动画时间长度正常，但实际渲染的帧数偏少，像被抽帧。"
> "PC 端当然明显更平滑，肉眼看不出卡顿，我当然是对比过才会提出这个问题。"

**不是**影片播放问题（`MOV/op00.mpg` 确实不被支持，但那是另一条独立的线，
见第 7 节），也不是分辨率/锯齿问题。

---

## 2. 量化结论（一句话）

**PC 上这段淡变是逐帧连续的（约 60～90 个台阶）；我们只有 8 个台阶，
每个台阶持续约 125ms。**

| | PC 录屏 | 本移植 |
| --- | --- | --- |
| 采样 | 1920×1080 @60fps，20s → 1201 帧 | 引擎帧缓冲 800×600 |
| 有可见变化的帧 | 154 / 1200（约 7.7 次/秒） | — |
| 单次变化幅度 | **约 0.37 / 255（逐帧微变）** | **32 / 255（8 级跳变）** |
| 淡变总台阶数 | 约 60～90 | **8** |
| 每级持续时间 | 约 16.7ms | **约 125ms** |

PC 录屏分析脚本：`local-data/analyze_pc_video.py`、`analyze_pc_segment.py`
（诊断脚本，未入库）。

---

## 3. 完整证据链（按因果顺序）

> **注意（2026-10-04 更正）**：下面的链条准确描述了「每圈约 128ms、每次跳 32 级」这一
> **观察**，但把它归因于 `force_wait()` 是**误判**——真正的根因是计时基点（见第 0 节）。
> 本段保留作为排查历史。

### 3.1 画面确实是"整屏线性淡变、每次跳 32 级"

我们在"内容哈希每次变化"时导出原始帧（PPM），16 张覆盖完整淡入淡出。
逐帧平均亮度：

```
#01 0.000 → #02 30.755 → #03 62.523 → #04 93.332 → #05 125.088
→ #06 156.865 → #07 187.666 → #08 219.416 → #09 245.455 → #10 214.632
→ #11 182.864 → #12 152.055 → #13 120.299 → #14 88.523 → #15 57.721
→ #16 25.971
```

每帧固定跳 30.8，全屏 600/600 行同步变化，文字内容不变。
→ 这是**定时淡变被切成了 8 个等分**，不是逐帧连续。

### 3.2 脚本只做了一个朴素的时间插值

用 `trace=1` 抓到 `SEEN9011` 的循环体（每轮执行一遍）：

```lisp
Line 0414: ReadExFrames({RAW : ( $ ff 00 00 00 00 $ 02 [ $ ff 01 00 00 00 ] )})
Line 0415: GetCursorPos(intA[1000], intA[1001], intA[1002], intA[1003])
Line 0415: goto_unless(intA[1002] == 2)
Line 0416: goto_unless(intF[1012] == 1)
Line 0416: index_series(intC[1], 0, 0, 2:{0, 10000, 255, 0})
Line 0421: objChildAlpha(170, 4, intL[0])
Line 0422: objChildAlpha(170, 5, intL[0])
Line 0423: objChildAlpha(170, 6, intL[0])
Line 0424: objChildAlpha(170, 7, intL[0])
Line 0425: refresh()
Line 0426: goto_unless(intC[0] == 0)
Line 0426: goto()
```

即：**读时间 → 线性插值出 alpha → 设给 4 个子对象 → 刷新 ← 循环**。
脚本意图完全正确。

### 3.3 脚本要的是 10000 级 / 1000ms

在 `InitFrame` 里打印参数，实测：

```
[InitFrame] counter=0 min=0 max=10000 time=1000     ← 8 级台阶那段
[InitFrame] counter=0 min=0 max=10000 time=1500     ← 12 级台阶那段
```

**10000 级、1000ms = 每级 0.1ms**，本该完全平滑。

### 3.4 插值公式本身是对的

`module_sys_index_series.cc` → `Sys_index_series::Adder()`：

```cpp
if (index > start && index < end) {
  int amount = endval - init;
  value += Interpolate(start, index, end, amount, mod);
}
```

`utilities/math_util.cc` 的 `Interpolate` 用 `double` 百分比计算，无量化。
`FrameCounter::ReadNormalFrameWithChangeInterval()` 里
`change_interval_ = milliseconds / abs(frame_max - frame_min) = 1000/10000 = 0.1ms`
——**公式正确**。

### 3.5 真正的问题：脚本每圈要 128ms 才转完

在 `FrameCounter::ReadNormalFrameWithChangeInterval()` 里打印每次读取：

```
[FrameCounter] ms_elapsed=128 num_ticks=1280 value=1280
[FrameCounter] ms_elapsed=0   num_ticks=0    value=1280   ×15
[FrameCounter] ms_elapsed=128 num_ticks=1280 value=2560
[FrameCounter] ms_elapsed=0   num_ticks=0    value=2560   ×15
...
```

**每 128ms 才真正推进一次时间**，期间 15 次空读。对照脚本循环体的指令数
（约 11～22 条）可知：**每轮主循环只执行约 1 条指令**。

同一个量在 `intC[1]` 上的表现（`intC[1] = 脚本的"已过时间"`）：

```
t=0ms     intC[1]=0      intL[0]=0
t=1496ms  intC[1]=1280   intL[0]=32
t=1618ms  intC[1]=2560   intL[0]=65
t=1757ms  intC[1]=3840   intL[0]=97
... （每级 120~140ms，步长固定 1280）
t=2383ms  intC[1]=10000  intL[0]=255
```

1000ms 的淡变只跑了 8 级 → 与第 3.1 节的 8 级台阶完全对应。

### 3.6 机制：`refresh()` → `ForceRefresh()` → `set_force_wait(true)`

```cpp
// src/modules/module_refresh.cc:37
AddOpcode(0, 0, "refresh", CallFunction(&GraphicsSystem::ForceRefresh));
```

```cpp
// src/systems/base/graphics_system.cc:270
void GraphicsSystem::ForceRefresh() {
  screen_needs_refresh_ = true;
  if (screen_update_mode_ == SCREENUPDATEMODE_MANUAL) {
    system().set_force_wait(true);        // ← 让本轮 exec 立即结束
  }
}
```

主循环（`app/src/main/cpp/native-bridge.cpp`）的字节码执行段原本是：

```cpp
do {
  machine.ExecuteNextInstruction();
  ++executed;
  now = system.event().GetTicks();
} while (!machine.CurrentLongOperation() && !system.force_wait() &&
         (now - slice_start < slice_ms));
```

`force_wait` 一旦置位，本轮立即退出 → 脚本循环被切成"每轮一小段"。
脚本每圈都调 `refresh()`，于是每圈要十几轮才转完 → 128ms/圈 → 8 级台阶。

`set_force_wait(true)` 的两个调用点：

- `graphics_system.cc:276`（上面这段，MANUAL 模式下的 `ForceRefresh`）
- `sdl_event_system.cc:169`（SDL 专用，本移植不经过）

---

## 4. 已排除的假设

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| 影片不支持导致（`MOV/op00.mpg`） | **是独立问题，但不是本症状** | 用户确认看的是实时渲染的告示/logo；且影片缺失表现为"整段不播"，不是"8 级台阶" |
| 软件合成太慢拖慢引擎 | 部分成立但不是根因 | 优化前后合帧 16ms→11ms/轮，`content changed` 仍是 8.5/s 不变 |
| 哈希采样太稀疏，漏检变化 | 否 | 采样密度从 1/128 提到 1/16，读数不变；且导出的原始帧本身就是 8 级 |
| 动画资源本身是 8fps | **否（关键）** | `InitFrame(..., max=10000, time=1000)` 明确要求 10000 级 |
| 对象动画用 GAN/ANM 帧序列 | 否 | 图形栈显示是普通 `Image` 对象（`TT_LOGO_WSA_*`），alpha 由脚本设置 |
| alpha 走渐进 mutator | 否 | `objChildAlpha` → `SetAlpha`（立即设置）；tree dump 里 `Mutators:` 为空 |
| 主循环轮率不足（55 轮/s） | 不是直接原因 | 即便 55 轮/s，若每轮能跑完一圈，台阶也应有 50+ 级 |
| `DriftGraphicsObject` 每轮标脏 | 真实存在但无关 | 它是"每轮强制重绘"的来源（见下），不影响台阶数 |

### 4.1 附带发现：`DriftGraphicsObject` 的节流计时器从不更新

```cpp
// src/systems/base/drift_graphics_object.cc:186
void DriftGraphicsObject::Execute(RLMachine& machine) {
  // We could theoretically redraw every time around the game loop, so
  // throttle to once every 100ms.
  int current_time = system_.event().GetTicks();
  if (current_time - last_rendered_time_ > 10) {     // 注释说 100ms，代码是 10ms
    system_.graphics().MarkScreenAsDirty(GUT_DISPLAY_OBJ);
  }
}
```

`last_rendered_time_` 只在构造函数里赋值一次，之后**从不更新**，因此该条件恒为真，
**每轮都标脏屏幕**。这是"每秒重绘 55 次、内容只变 8 次"的浪费来源之一
（上游缺陷，未修改——怕影响画面正确性，需先确认 Drift 对象是否真的每帧都在动）。

---

## 5. 复现方法

### 5.1 诊断开关（写入
`/sdcard/Android/data/org.rlvm.android/files/rlvm-diag.txt`）

```
frame_log_every=1000
dirty_gate=1
dirty_stats=1
```

可选：

| 开关 | 作用 |
| --- | --- |
| `trace=1` | 打印每条字节码指令（`rlvm-stderr` tag），日志量大 |
| `dump_graphics=1` | 内容变化瞬间输出图形栈（含每个对象的 alpha） |
| `dump_frames=N` | 每次内容变化导出 PPM 原始帧到诊断目录（最多 N 张） |
| `slice_ms=N` | 主循环时间片（默认 6ms） |
| `exec_ignore_force_wait=0` | **退回 `force_wait` 旧行为**做对照（见 5.3） |

### 5.2 关键日志行

```
intC1 -> 1280 (intL0=32, t=1496ms)     ← 时间变量每 ~125ms 跳一格（核心证据）
vars: intC[0]=0 intC[1]=2560 intL[0]=65
exec detail: ops=60 slowest=0.0ms scene=9011 line=453   ← ops≈轮数 = 每轮 1 条指令
long op: rounds=60 of 60 loops
phase cost: run=0.03ms exec=0.00ms wait=6.17ms per-loop
```

### 5.3 未验证的修复（已实现，默认开启）

`native-bridge.cpp` 里把 `force_wait` 从 exec 退出条件中移除：

```cpp
} while (!machine.CurrentLongOperation() &&
         (!system.force_wait() || diag.exec_ignore_force_wait) &&
         (now - slice_start < slice_ms));
```

理由：`force_wait` 是上游 **SDL 后端**的节奏控制（`refresh` 时让出时间片给 SDL
视频刷新）。本移植的合帧由主循环每轮统一完成，`force_wait` 唯一的实际效果
就是把脚本循环切碎。默认 `exec_ignore_force_wait=true`。

**该修复未做真机验证**（用户叫停）。验证方式：跑开屏淡变，看
`intC1 ->` 的间隔是否从 ~125ms 降到接近帧长；以及 `exec detail: ops` 是否显著上升。

---

## 6. 尚未查清的点（接手者建议从这里继续）

> **2026-10-04 更新**：本节第 1、2 条已由第 0 节的根因（计时基点）回答——`force_wait`
> 与 `CurrentLongOperation()` 都**不是** 8 级台阶的原因。第 3、4 条仍然成立。

1. **`force_wait` 是不是唯一因素。** 修复后若台阶仍粗，需查
   `machine.CurrentLongOperation()` 是否每轮非空（长操作同样会打断 exec，
   而 `long op: rounds=60 of 60` 显示它确实每轮都挂着）。
2. **PC（SDL 后端）为何能达到 60～90 级。** 上游 `RLVMInstance::Run` 同样是
   "执行到 `force_wait` 就跳出时间片"，理论上也该被切碎。需要有人对照 SDL 端
   实测 `initC[1]` 的更新粒度，判断差异来自 `force_wait` 还是别处。
3. **`Refresh()` 是否够快。** 本轮优化后单次合帧仍约 11ms（`blit path: fast=60
   slow=540 calls`，其中 slow 是逐像素 alpha 混合，约 20ns/像素）。若最终要求
   120Hz 下每帧都能刷新，这块还需要 SIMD（NEON）或减少图层。
4. **`DriftGraphicsObject` 的节流缺陷**是否该修（见 4.1），需要先确认 Drift
   对象是否每帧真的在动。
5. **`exec_ignore_force_wait` 是否还需要保留。** 它**并非根因**（修复计时基点后，
   即使仍忽略 `force_wait` 也已平滑）。建议用 `rlvm-diag.txt` 的
   `exec_ignore_force_wait=0` 做一次 A/B，比较观感与 `content changed` / `loop dt`，
   再决定保留还是还原成上游行为。

---

## 7. 与本问题无关但已确认的另一条线

`MOV/op00.mpg`（Kud Wafter 90MB、LBEX 223MB）是两部作品的开场影片，
而 `src/modules/module_mov.cc` 把 `movPlay / movPlayEx / movLoop / movWait /
movPlaying / movStop / movPlayExC` **全部注册为不支持指令**：

```cpp
MovModule::MovModule() : RLModule("Mov", 1, 26) {
  AddUnsupportedOpcode(0, 0, "movPlay");
  ...
}
```

因此开场影片在 RLVM 上**根本不播**。这是需要另立一项的功能缺口
（Android 侧可用 MediaCodec 解码后灌进帧缓冲），与本报告的台阶问题无关。

---

## 8. 本轮实际落入代码的改动

| 文件 | 改动 | 作用 |
| --- | --- | --- |
| `app/src/main/cpp/native-bridge.cpp` | 主循环忽略 `force_wait`（带开关） | 第 5.3 节；**非根因**，是否保留待 A/B（见 6.5） |
| `app/src/main/cpp/android/android_system.cpp` | `NowMillis()` 改为**相对进程起点**的毫秒（对齐 `SDL_GetTicks()`） | **第 0 节根因修复**：8 级台阶 → 92~123 帧/秒，已真机验证 |
| `app/src/main/cpp/android/android_graphics.{h,cpp}` | 整面不透明快路径（`pixels_opaque_` 传播）+ `memcpy` 分支 | 合帧 16ms→11ms/轮 |
| `app/src/main/cpp/native-bridge.cpp` | `CaptureFrame` 只在内容哈希变化时调用 | GL 线程不再空转重传 1.9MB 纹理 |
| `docs/DECISIONS.md` | D-023 | 本报告的决策记录 |

诊断探针（`index_series`、`memory_intmem`、`module_sys_frame`、`frame_counter`
里的 `std::cerr`）已全部回退，`git diff` 干净；仅保留
`graphics_object.cc` 里受 `dump_graphics` 开关控制的 alpha 输出。
