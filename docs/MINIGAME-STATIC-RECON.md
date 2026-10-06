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

## 3. 真机核对（2026-10-06 夜，动态调试第一轮）

装上新包后逐项核对 §0 的两处修复与滚轮通路：

| 指标 | 结果 |
| --- | --- |
| `Tried to call empty RLOp_SpecialCase::Dispatch()` | **0 次**（Dispatch 修复生效，每帧异常消失） |
| `[lb-ext] objbtn hit#` / `objbtn poll` | **0 次**（每帧 stdout 门控生效） |
| `rlvm-input: wheel delta=…` | **48 次**；用户确认「滚轮映射正常」→ **log/回想在真机上第一次打得开** |
| `未实现指令` / `步数上限` / `HOST FAULT` | 0 / 0 / 0 |
| 引擎节奏（VN 段） | `progress: frames=3420…3720`，约 **45–50 fps**（小游戏段待下一轮单独测） |

## 4. Sys opcode 覆盖度审计（新工具 `tools/sys_opcode_coverage.py`）

用现成文件（`build/rlvm-scenes.txt` + 上游 `module_sys*.cc` + 平台层 `native-bridge.cpp`）
回答「脚本实际用到的 Sys opcode 里，哪些两边都没实现」：

```
# 脚本用到 195 种 (opcode, overload) 组合，累计 101661 次
# 上游 module_sys*.cc 注册 287 组；平台层补 21 组
## 平台层补的桩（脚本在用）
  Sys 211 ov=0  37131 次      Sys 216 ov=0  37131 次
  Sys 210 ov=0   7875 次      Sys 215 ov=0   7875 次
  Sys 150 ov=0     21 次
## 两边都没有（引擎打 Undefined 后跳过）
  Sys 366 ov=0  61 次   Sys 801 ov=0 57 次   Sys 457 ov=0 56 次
  Sys 456 ov=0  50 次   Sys 106 ov=0 43 次   Sys 2502/2402 ov=0 各 39 次
```

**结论**：

1. 除了 `210/211/215/216` 这一族（合计约 **9 万次**，疑似等待/同步类，语义待 D-040 标定），
   其余缺口都是**低频**（≤61 次），影响面小。
2. `Sys 456` 是明确的**注册遗漏**：我们只注册了 overload 1，脚本用的是 overload 0 →
   下个包顺手补齐；`366 / 801 / 457 / 106 / 2502 / 2402` 一并按现有做法挂 ignore 桩
   （效果与现在的 Undefined 跳过相同，至少不再刷日志）。
3. 审计口径写进工具里，之后每加/改一个 Sys op 都可以重跑一次看覆盖度。

## 5. 真机性能基线（2026-10-06 夜，摘掉每帧异常与每帧 I/O 之后）

同一次运行（用户进小游戏、不操作、跑约 4.5 分钟），从 `build/full26.log` 统计：

| 指标 | 数值 |
| --- | --- |
| 引擎帧循环 | 窗口帧率 **median 51.4 fps**（217 个窗口；最低 11.5 fps 的个别窗口=一次卡顿，最高 183 fps=卡顿后的追赶） |
| 渲染帧 | `frame content hash` 的 `serial 63 → 13215` / 278.3 s → **≈47.3 fps** |
| `[pt00] frame` 快照 | 14 条（小游戏推进到约 **frame 660**，`42/44/45=43` 等数值在推进） |
| `未实现指令` / `步数上限` / `HOST FAULT` / `Tried to call empty` / `objbtn` 日志 | **全 0** |
| `Undefined: opcode` | 53 行，全是低频（每个 op 出现 1–3 次：`461ov1` `460ov1` `300` `2056` `201` `432` `1231` `437` `442` `447` `255:12/15` …） |
| 场景轨迹 | 小游戏段（`7010/7020/7300/7921/7922/7940`）→ **`SEEN513` 等剧情场景**（说明第一场结束后剧情接上了） |
| RSS | 285–340 MB，无爬升趋势 |

**判读**：卡顿的"自身来源"（每帧异常 + 每帧 stdout + 每帧 Dispatch 空实现异常）已经摘掉，
整体节奏到了 40–50 fps 量级；剩下的抖动更像**瞬时卡顿**（个别窗口 11.5 fps）。
下一步要么是 `210/211/215/216` 这一族的时序语义（真值标定后再实现），
要么是逐帧耗时三段拆分（执行器 / 渲染 / 合帧）——都需要先有用户的主观描述来定位"哪一刻卡"。

## 6. 卡顿根因：debug 构建把渲染器也编在 `-O0`（2026-10-06 夜，已修）

### 6.1 怎么量到的

`blit_cost=1`（本轮新增的 diag 键）让引擎每 60 帧打一行合成成本：

```
# 用户描述：「运镜和球飞出去、也就是左边视窗世界地图基线变动时卡」
blit: calls=174 pixels=54240000 time=1086.0ms worst=42.3ms  0,0 800x600 -> 0,0 src_surface=800x600
blit: calls=62  pixels=0        time=280.0ms  worst=8.8ms   0,0 800x600 -> 0,0 src_surface=800x600
```

* 重的时刻**光合成就吃满整秒**（1086ms/秒，5400 万像素 ≈ 113 个全屏）；
* 90ns/像素级别的逐像素 alpha 混合 —— 比正常慢一个数量级；
* 还有一类"没写出像素（`pixels=0`）却每次 4.7ms"的调用（60 次 = 280ms/秒）。

（补充：探针 `TakeBlitCostSummary` / `TakeBlitWorstSummary` 其实**早就写好了，但没有任何
消费者**，计数器一直空转 —— 这轮才把消费者接到引擎的周期日志上，所以"卡在哪一层"从来没被量过。）

### 6.2 根因与修法

debug 包把**整份 `librlvm` 编在 `-O0`**；此前只有 PT00 执行器单文件开了 `-O2`
（`CMakeLists.txt` 那段的注释原话就是"debug 构建默认 -O0 时它是整场卡顿的主因"），
**渲染器没有跟着开**。于是 CPU 逐像素混合一直是 O0 速度，像素量一大（运镜重采样世界地图、
球飞带出一批 sprite）就顶满 CPU。

修法（同样只动热路径，保留其余代码的可调试性）：

```cmake
set_source_files_properties(
    android/android_graphics.cpp   # CPU 合成/贴图
    android/android_system.cpp     # 每帧刷新/事件循环
    android/font_engine.cpp        # 字形光栅化
    PROPERTIES COMPILE_OPTIONS "-O2")
```

已在 ninja 的编译命令里确认 `-O2` 生效。**A/B（同一条路线、同样的 blit_cost）**：

| 指标 | -O0 | -O2 |
| --- | --- | --- |
| 同样 60 次合成的窗口 | 280.0ms（worst 8.8ms） | **30.9ms**（worst 0.6–1.5ms）≈ **9×** |
| 重负载窗口 | 1086ms / 54.2M 像素（50M px/s） | 最大 671ms / 95.7M 像素（**≈143M px/s**，≈2.9×） |
| 单次最慢合成 | 42.3ms | 0.6–1.5ms |

用户确认：**「确实流畅多了」**。

### 6.3 关于"不是有 GPU 适配层吗"

我们确实有一层 GL，但它**只负责"上屏"**：引擎每帧把全部图层在 **CPU 像素表面**里合成完
（`AndroidSurface` + `BlitToSurface`：图层顺序 / alpha 混合 / GRP 区域表蒙版 / 缩放 / 裁剪），
再把整帧上传成纹理、由 GL 画一个四边形（含 letterbox 与缩放）。视频通路
（MOV → AMediaCodec → GL/SurfaceTexture）是唯一把解码帧直接经 GL 上屏的地方。

也就是说：**合成（也就是刚才 1086ms/s 的那部分）全在 CPU**，GPU 只省了"把成品贴到屏幕"。
之所以这么做：上游 RLVM 0.14 **没有 GL 后端**（`src/systems` 只有 `base` 与 `sdl`），
它的 `Surface::RenderToScreen` 语义（G00/GRP 区域表、mask、per-object 色调、clip）
在 SDL 后端本来就是 CPU 混合 —— 我们照同一语义实现，保真度最容易对齐
（这条正好解释了后来"全黑根因 = `GetPattern` 未实现"的教训）。

把合成搬到 GPU **不是"启用已有开关"，而是新写一条渲染管线**：需要在 GL 侧实现
区域表 / 蒙版 / 色调 / 裁剪的等价物，并保证与 CPU 路径逐像素可比 —— 与上游之间没有可直接
借用的实现。**因此登记为 v0.3.x 的可选性能项**（当前 -O2 之后用户主观已"流畅多了"，
不构成阻塞）。若要进一步省 CPU，更便宜的方向是**减少像素量**（静态层缓存 /
只重绘变化区域），而不是立刻重写渲染后端。

## 7. 「丢背景 / 实体不渲染」定位（2026-10-06 夜，真机取证）

### 7.1 取证方式

用户停在问题帧 → 面板「导出渲染树」（`MainActivity.kt:266`，引擎线程按需导出到应用日志）
→ 同时 `adb shell screencap` + `adb pull` 取画面。两样对照即定位。

### 7.2 现象与判定

* 画面：**左视窗（世界地图那一块）是空白**，右视窗正常；部分迷你角色缺失。
* 整份渲染树（601 行）里 **`PT_MAP*` 出现 0 次**，而 `PT_MORI00?`×4、`PT_KABE00`×8、
  `PT_WOOD00?`×2、`PT_BALL01`×8、`PT_KAGE00`×39、玩家 GAN 全都在 ✓
  → 缺的不是渲染，是**那个对象不在栈里/没有数据**。
* **更精确的结论**：对比「第 1 轮自动 dump（frame 150）」与「第 2 轮手动导出」，
  同一批对象变成 **`vis=0 data=0`**：

| 对象 | 第 1 轮 | 第 2 轮 |
| --- | --- | --- |
| #201 | `PT_MAP00?`（世界地图） | `vis=0 data=0` |
| #213 / #216 / #219 / #220 | `PT_STOP00` / `PT_CALL00` / `PT_SKIP00` / `PT_SKIP_CAM00`（小游戏 UI 按钮） | `vis=0 data=0` |
| #230 | `KURO`（黑场层） | `vis=0 data=0` |
| #232 | (无图像) | `CGKS11`（这一轮新挂上的） |

* 而 `PT_MAP00?` 在第 2 轮**确实又被加载过两次**（`loaded "PT_MAP00?"` @17:35:17 与
  17:35:44）→ 说明 `objOfFile(201, …)` 跑到了（装载发生了），但**建立的效果没有留在对象上**，
  或者建立之后又被清掉。
* 脚本侧的建立序列（`SEEN7010`）：`1:61:10(201)` → `1:81:1000(201,0,0)` →
  `1026(z)` → `1039` → `1034 ov1(objDispRect, intD[310..313])` → `1004(vis=1)` →
  `1:71:1000(201,"PT_MAP00?")`（= `objOfFile`）→ `goto` → 再 `PT_MAP01?` / `PT_MAP02?`。
  其中 `1:71:1000` = `objOfFile`、`1:71:1500` = `objOfChild`、`1:81:1034 ov1` = `objDispRect`
  —— 都是**上游已实现**的 opcode，不是我们的桩。

### 7.3 下一步

用 `tools/rlvm-diag.minigame-trace.txt`（`lb_minigame=1` + `trace=1`）抓**第二轮开始前后**的
逐指令流，看：

1. `SEEN7010` 那段建立序列在第二轮是否**完整执行**（有没有 `goto` 跳掉一段）；
2. 在 `objOfFile(201, …)` **之后**，还有哪条 op 动过 201（或整批对象）；
3. 我们挂的容错桩里是否**吞掉了其中一条**（候选：`ChildObjFg 1058`、`GanFg 101/102`、
   `Sys 210/211/215/216`、`Sys 431/460/461`）。

---

## 9. 已结案：运镜结束时地图（objFg #201）消失（2026-10-07 凌晨）

**症状**：运镜（每帧 `objMove(201, intD[316], intD[317])`）结束、CG 切入
`bgrLoadHaikei("?", 230)` 的那一刻，左视窗里的 `PT_MAP00?`（objFg #201）消失。

**根因**：`GraphicsObject::Impl` 的**拷贝构造函数**里写死 `wipe_copy_(0)`
（同一文件里的赋值运算符反而正确拷贝了它）。`MakeImplUnique()` 正是用这个
拷贝构造产生私有副本，于是**任何参数 setter 的首次写入**都会把
`objFgWipeCopyOn` 打的保护标记静默清零：

| 步骤 | 现场证据 |
| --- | --- |
| 1. `SEEN7120 Line 0223` 保护 200..249 | `objs` 时间线：200..249 全部 `w1` |
| 2. 同帧常规参数更新（`objEveDisplay(213,…)`、`objMove(201,…)`…）触发 `MakeImplUnique()` | ~100ms 后**只有** 201/213/214/216/217/219/220/230 掉回 `w0`，恰好是那一帧被 setter 碰过的那批 |
| 3. 运镜末尾 `bgrLoadHaikei("?",230)` → `ClearAndPromoteObjects` | `[wipe] #201 fg(alloc=1 data=1 wc=0) -> FG_FREE`；汇总 `fg_freed=110` |

`savepoint` 快照也要算进来（`LazyArray::CopyTo` 用 GraphicsObject 拷贝构造共享
`impl_`，之后的首次 setter 同样丢标记）——所以保护“看起来打上了”，却活不过一帧。

**修复**：`rlvm-release-0.14/src/systems/base/graphics_object.cc` 的 `Impl`
拷贝构造改为 `wipe_copy_(rhs.wipe_copy_)`（+9/-1，只此一处）。中途试过的
“`InitializeParams` 保留 `wipe_copy`”已回滚——无效，因为标记是在其**之后**的
setter 里丢的。

**真机 A/B**（同一复现路径，`wipe_log=1`）：

* 修复前：保护循环后 ~100ms，201 变 `vdw0`，随后 `--w0`；运镜末尾晋升汇总
  `fg_alloc=110 fg_had_data=17 fg_freed=109/110`。
* 修复后：201 一路保持 `vdw1`（连续 15 秒无变化）；运镜末尾晋升汇总
  `fg_alloc=110 fg_had_data=17 fg_freed=60 bg_alloc=6 bg_promoted=6`
  —— 受保护的 50 个槽位不再被擦，地图不再消失。

**新增只读取证设施**（默认全关，示例见 `tools/rlvm-diag.minigame-wipe.txt`）：

| diag 键 | 作用 |
| --- | --- |
| `wipe_log=1` | `ClearAndPromoteObjects` 逐对象打印 `fg(alloc/data/wc) bg(alloc/data) -> FG_FREE / fg_keep / BG_COPY` 与一行汇总；并打印 195..255 号对象的活体时间线（可见/有数据/WipeCopy 变化才打一行） |
| `op_trace=a,b,c` | 逐指令 trace 的白名单过滤器（`rlmodule.cc`；过滤器为空时行为与原来完全一致），避免 46MB 全量日志 |

**遗留（独立问题，未修）**：同一次晋升里还有
`#232 fg(data=1 wc=0) bg(alloc=1 data=0) -> FG_FREE BG_COPY` —— `bg` 槽位一旦
用过就永久处于“已分配”，即使里面已经空了，之后每次晋升都会 `fg = bg` 把空对象
盖到前景槽位上，已经显示出来的 CG 会被弄丢。修法（只在 bg 真有内容时才覆盖 fg）
会改变系统消息窗口那条路径的行为，需要单独决策 + A/B。

---

## 10. 场景运行时流程重建（2026-10-07 凌晨，静态 + 真机日志对照）

### 10.1 前提：`#entrypoint N` 不是 farcall 用的 id

`dump_scenario` 打印的 `#entrypoint N` 是**元数据里的原始值**（= kidoku 表下标），
真 id 是 `kidoku_table[N] - 1000000`（见 `libreallive/bytecode.cc:273`）。
新增离线工具 `tools/seen_entrypoints.py`（读 `Seen.txt` 的 TOC + 头里的 kidoku 表）：

```
SEEN7111  kidoku 6 项：索引 0..4 -> id 0,1,2,3,4；索引 5 -> id 10 ★
SEEN7410  kidoku 13 项：索引 0..3 -> id 0..3；索引 4..12 -> id 10..18
SEEN515   索引 0 -> id 0，258 -> 10，1118 -> 15，1138 -> 89
```

### 10.2 棒球小游戏的调用图（脚本静态）

| 场景 | 角色 | 关键调用 |
| --- | --- | --- |
| `SEEN513` | 棒球线入口 | `farcall(7111, 0)`（清能力表 + 建场） |
| `SEEN515/516/518/519/520` | 比赛/练习剧情 | `farcall_with(7030, 0\|1\|2\|3, intF[1371], intF[1981], <模式>)`，每段后 `farcall(7111, 10)`（名册）/`farcall(7111, 4)`（结算） |
| `SEEN517` | 练习菜单 | `7030 ep0` 带模式 `20/21/30/31/32`；`farcall(7410, 17 / 11)` |
| `SEEN7030` | 小游戏总控 | ep0..ep3 四段；写 `intD[72]=intF[1371]`、`intD[74]=intF[1981]`、`intD[73]=模式` |
| `SEEN7110` | 赛季初始化 | 按 `intD[73]` 设 `intD[70]/[71]/[76]/[79]/[80]/[81]/[712]/[750]` |
| `SEEN7111` | 球员数据 + 名册画面 | id0 清能力表 + `farcall(7410, 10)`；id1/id2 能力值 ↔ `intD[900+i*7+j]`；id3 成长分配；id4 结算；**id10 = 建名册画面**（`PT_PR_BG00/01/02` + 220+i 头像/背号/箭头 + `farcall(9030, 0)`） |
| `SEEN7410` | 建场（背景/实体） | id10 = 初始化；id11/id17 被 `SEEN517` 调用 |
| `SEEN7420` | 场内主循环 | 读 `intD[101]`（挥棒）、`intD[103..107]`（走动/跑步）、`intD[610..612]`（相机） |
| `SEEN9030` | 「背景 + 最多 3 行文字」通用助手 | `objBgInit(100/110/111/112/120)`、`grpLoadHaikei(strS[1000])`、`farcall(8720, 22)` |

模式值（`SEEN7110` ep0 实见）：

| 模式 | 效果 |
| --- | --- |
| `0/1/2` | 场内自由走动（`intD[70] -= 3`；不设 `intD[76]`） |
| `10/11/12` | 投球：`intD[76]=1/2/3`，并改 `intD[70]/[71]`（球数） |
| `20/21` | 球数减半（`intD[71]=intD[70]/2` 或 `intD[70]/=2`） |
| `30/31/32` | 打击练习：`intD[76]=4/5/6`、**`intD[80]=1`**、`intD[712]=0/1/2`、`intD[79]=1`、`intD[81]=1` |

### 10.3 【重要】名册画面（7111 id10）在真机上**从未执行**

证据（扫 `build/` 下全部日志）：

* `PT_PR_BG00/01/02`、`PT_PR_YAA00…`、`PT_PR_NUM00`、`PT_PR_UP00` **一次都没出现在
  图像加载日志**（`loaded "%s"`，`android_graphics.cpp:829`）。同一批日志里
  `PT_YAA00…PT_YJA00`、`PT_YKA00/YMA00/YNA00/YLA00..08`、`PT_BALL01`、`PT_MAP00?`
  都正常出现 → 场内资源加载没问题，只有名册这套从未被请求。
* id10 的前三条指令 `Sys 336 / 359 / 1212` 在本机是 `[lb-ext]` 桩 / 未实现，执行到一定会
  打 `[lb-ext] Sys336 … not implemented yet` 或 `Undefined: opcode<1:4:336>`；
  两者在所有日志里**都没有**。
* 对照：`(SEEN7111)` 的报错行只落在 Line 0017..0128 = id0/id1/id2/id3；`(SEEN515)`
  只到 Line 2163。

→ 手机上目前跑到的是「7111 id0-3 + 7030 场内主循环」这一段；名册这条链路
`SEEN515 → 7030 ep0 → 7111 id10 → 7030 ep1 → 7111 id4` 从没走通。

### 10.4 上一轮结论的更正

「`SEEN7111` 菜单代码没执行」**结论对、理由错**：当时以为 `Sys 210/211/215/216` 没实现
就说明没跑过，实际这些 op 在本机是 `[lb-ext]` 桩（打 `Sys210 … not implemented yet`），
**它们确实执行过**（剧情块里的背景设置）。真正判定 id10 没跑的证据是 §10.3 的
`PT_PR_*` 从未加载 + `Sys 336/359/1212` 从未出现。
