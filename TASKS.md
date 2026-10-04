# TASKS.md — rlvm-android 任务清单

来源：`task.md`。本文件是执行视图，状态与偏差以 `docs/DECISIONS.md` 为准。

状态图例：`DONE` 已完成并验证 / `WIP` 进行中 / `TODO` 未开始 / `BLOCKED` 阻塞

## 阶段 0：认知与仓库初始化

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T0.1 | 只读分析 RLVM 源码 | `docs/ARCHITECTURE.md` | 模块列表完整、依赖清单准确 | **DONE** |
| T0.2 | 生成 Android 项目骨架 | Gradle KTS、CMake、MainActivity、Manifest | `./gradlew assembleDebug` 通过 | **DONE**（离线构建，49s / 迁移后 1m43s） |
| T0.3 | 写入 AGENTS.md 与 TASKS.md | `AGENTS.md`、`TASKS.md` | Agent 启动时能自动读取 | **DONE** |

## 阶段 1：原生层编译适配

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T1.1 | Android 化 CMakeLists（接入 RLVM 上游源码） | `app/src/main/cpp/CMakeLists.txt` | CMake 配置无错误 | **DONE** |
| T1.2a | `boost::filesystem` → `std::filesystem` | 上游改动 | 编译通过 | **暂缓**：已用 Boost 官方开关绕过 C++20 依赖，无需为编译而迁移；是否迁移取决于存档兼容性（见 D-011） |
| T1.2b | 桌面依赖替换（SDL 相关的音频/渲染/文本见阶段 4） | 依赖替换方案 + 代码 | 缺失依赖逐步减少 | 部分完成：SDL 音频转换用 shim 顶替（D-012），SDL 渲染/事件/文本仍待阶段 3/4 重写 |
| T1.3 | 编译迭代修复 | 可编译的 `librlvm.so` | `externalNativeBuildDebug` 通过 | **DONE**：两 ABI 均通过，APK 21.6 MB |
| T1.4 | 最小 JNI 验证 | `native-bridge.cpp` | App 能调用 native 并返回结果 | **DONE**：真机（Redmi K40 / Android 12）上跑通 Gameexe 解析 + SEEN TOC + Scenario 构造，详见 dev-log/T1.4.jsonl |

## 阶段 2：文件系统与 JNI 桥接

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T2.1 | JNI 接口设计 | `NativeBridge.kt`、`native-bridge.cpp` | 方法可被调用，无动态查找 | TODO（骨架已建立 RegisterNatives 模式） |
| T2.2 | SAF 文件访问（`android-fs`） | `android/saf_file_system.*`、`jni_saf_backend.*`、`SafFileSystem.kt` | 能读取 `SEEN.txt` | **DONE**：SAF 下读 Gameexe.ini + 以 fd 打开 SEEN.TXT（mmap），引擎跑出与普通路径一致的结果 |
| T2.3 | 触摸事件映射 | Kotlin 触摸监听 + JNI | 游戏内光标可移动、点击 | TODO |
| T2.4 | 引擎生命周期 | JNI + Kotlin 封装 | 能启动、暂停、恢复、退出 | 部分完成：引擎已在真机上装配并执行字节码（6 条指令 + 文本长操作），暂停/恢复/退出待补 |

## 阶段 3：Compose UI 与 SAF

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T3.1 | SAF 目录选择器 | `MainActivity` 内的选择器 | 重启后仍可访问目录 | **DONE**：ACTION_OPEN_DOCUMENT_TREE + takePersistableUriPermission + SharedPreferences（与 T2.2 一并落地） |
| T3.2 | 游戏库与设置 UI（**在此引入 Compose + Material 3**） | `MainScreen.kt`、`GameViewModel.kt` | 界面可交互 | TODO |
| T3.3 | GLSurfaceView 集成 | `RlvmRenderer.kt` | 游戏画面能显示 | **DONE**：呈现链路打通（CPU 合成 → 帧缓冲 → JNI → GL 纹理 → 屏幕），实测显示正确 |

## 阶段 4：音频与渲染

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T4.1 | AAudio 音频（原计划用 Oboe，已改，见 D-004） | `android/audio_engine.*` | 语音播放正常，无爆音 | **DONE（首版）**：44.1kHz WAV 经 SAF 播放，峰值与夹具一致；NWA/OGG 已接线待真机素材验证 |
| T4.2 | 渲染管线重写（OpenGL 1.x 固定管线 → GLES 3.0） | `gl-renderer.cpp`、`GameRenderer.kt` | 画面正常，无花屏 | TODO |
| T4.2a | 图像加载（GRP/PDT/G00 解码接入 + 经 GameFileSystem 读取） | `AndroidGraphicsSystem::LoadSurfaceFromFile` | 能加载并显示真实图像 | **DONE**：SAF 下解码 320x240 BMP 并正确合成显示，通道序已修正 |
| T4.3 | 同步与性能 | 优化补丁 | 视觉小说场景稳定 60fps | TODO |

## 阶段 5：Android 12+ 系统适配

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T5.1 | 前台服务 `mediaPlayback` | `GameService.kt`、Manifest | 后台运行不被杀 | TODO |
| T5.2 | 组件导出（`android:exported`） | Manifest | Android 12+ 安装无错 | DONE（启动 Activity 已声明 `exported="true"`，后续新增组件需继续核对） |
| T5.3 | PendingIntent 与休眠 | 代码修正 | 无系统级崩溃 | TODO |

## 阶段 6：测试、优化与发布

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T6.1 | 单元/仪器测试 | 测试文件 | 测试通过 | TODO |
| T6.2 | Profiler 优化 | 优化报告 + 补丁 | 帧率、内存达标 | TODO |
| T6.3 | 发布配置 | `release/` 文档 | 可生成 AAB | TODO |

## v0.2.1 发布记录（2026-10-04）

> 版本策略：0.1.0 = 首个可用版；0.2.0 = 黑屏挂起 + 文本容器切换 + 中文数据兼容。
> 0.2.1 聚焦"操作完整性"：先把点击补齐，再把按键通道做出来（默认不启用）。

**发布状态：已发布。** tag `v0.2.1` = 提交 `bb07128`，Release 附 `app-release.apk`。

- 仓库：https://github.com/Tooke258/rlvm-android
- Release：https://github.com/Tooke258/rlvm-android/releases/tag/v0.2.1
- 实际内容比预定计划多两项：**实时动画平滑度的根因修复**（D-023）与
  **存档链路收口**（D-024 还原 / D-025 通用性边界）。

### v0.2.2 实际内容：回滚到 v0.2.0 + 计时修复，再增量回归到功能齐平（2026-10-04）

**发布状态：已发布。** tag `v0.2.2` = 提交 `93e99b5`，Release 附 `app-release.apk`（Latest）。

- Release：https://github.com/Tooke258/rlvm-android/releases/tag/v0.2.2
- v0.2.1 因过场黑闪被撤回，本版在**定位并修复**该问题后重新发布（根因见下）。

**最终状态（真机验收）**：平滑 ✓、**无过场黑闪** ✓、按键输入 + 界外点击 ✓、
合帧走 blit 快路径 ✓，性能优于 v0.2.0 与 v0.2.1。

**黑闪根因（已定位并修复）**：D-022 的**脏标记闸门**使合帧只在被标脏时发生，而
`BeginFrame()` 每帧先把帧缓冲填成不透明黑、再由 `DrawFrame()` 把 DC0 blit 回去——
某次合帧若落在「DC0 已清空、内容未画上」的瞬间，这张黑帧会一直挂到下次标脏
（实测整屏黑 200ms~3s）。修法：**闸门默认关闭（每轮合帧），保留 blit 快路径**。
详见 `docs/DECISIONS.md` D-026 及其补充节。

**增量回归顺序（每步真机验黑闪）**：

| 步骤 | 内容 | 黑闪 |
| --- | --- | --- |
| 1 | 重加 T7.2（按键通道 + 浮动按键栏） | 无 |
| 2 | 重加 T7.1（界外点击映射） | 无 |
| 3 | 重加 D-022 → 触发 | **出现** |
| 3.1 | 关 `dirty_gate`、留 `blit_fast` | **消失** |
| 3.1 | 关 `blit_fast`、留 `dirty_gate` | 仍闪 → 像素路径无关 |

**未回归的（刻意留下）**：`CaptureFrame` 的内容哈希闸门、GL 侧的"先问帧号提前返回 +
`glTexSubImage2D`"、诊断工具 `loop_probe` / `dump_scenario`（实现存于 `git stash@{0}`）。
前两者是 v0.2.1 里可能独立造成黑闪的路径，未再逐一验证；诊断工具可按需再摘回，
但**摘回时必须连带验证黑闪**。

**发布前状态变更**：v0.2.1 之后真机发现**过场整屏黑闪**（KW 与 LBEX 都有，v0.2.0 实测没有）。
对 6 个可疑变量做**单独** A/B（blit 优化 / 脏标记闸门 / `exec_ignore_force_wait` /
计时基点 / GL 呈现路径 / `CaptureFrame` 哈希闸门）**全部无效** → 判定为**耦合回归**。
于是**整体回滚到 v0.2.0，只保留计时基点修复**（唯一被证明的真根因修复）。真机验收：平滑 + 无黑闪。
完整记录见 `docs/DECISIONS.md` **D-026**。

**回滚去掉的（后续按需重加，**每个功能一提交、每步真机验收黑闪**）**：

| 丢掉的功能 | 优先级 | 说明 |
| --- | --- | --- |
| T7.2 按键通道 + 浮动按键栏 | **高** | LBEX 这类需要键盘输入的作品拿不到输入 |
| T7.1 黑边点击推进对白 | 中 | 点画面外的黑边不再推进文字 |
| D-022 合帧优化 | 中 | 合帧回到较慢路径（约 16ms/轮） |
| `loop_probe` / `dump_scenario` / `blackframe_probe` 等诊断 | 中 | 实现存于本地 `git stash@{0}` |

### v0.2.3 计划（主线：**视频通路** —— `MOV/*.mpg` 播放）

> 决定（2026-10-04）：小游戏 / DLL 那条线**后移**到 v0.2.4 或 v0.3.0，v0.2.3 先做**视频通路**。
> 这里说的"视频"是**影片播放**（`MOV/*.mpg`），与"实时动画渲染"（D-023 已修）是两回事。

**现状**：`src/modules/module_mov.cc` 的 7 条指令**全部注册为 `AddUnsupportedOpcode`**：

| 指令 | opcode | 现状 |
| --- | --- | --- |
| `movPlay` | 0 | 未实现（空操作） |
| `movPlayEx` | 1 | 未实现 |
| `movLoop` | 2 | 未实现 |
| `movWait` | 3 | 未实现 |
| `movPlaying` | 4 | 未实现 |
| `movStop` | 5 | 未实现 |
| `movPlayExC` | 20 | 未实现 |

后果：开场影片（LBEX 的 `MOV/op00.mpg`、KW 的 MOV 资源）在 RLVM 里**完全不播**。
这是**独立的功能缺口**，与帧率/动画平滑度无关（见 `docs/FRAMERATE-INVESTIGATION.md` 第 7 节）。

**方向**：Android 侧用 **MediaCodec** 解码 + 把帧回填进引擎的 Surface（再由 GL 侧呈现），
把上面 7 条指令接到这条通路上；其中 `movWait` 需要与引擎的长操作/时序配合
（可参考 `long_operations/wait_long_operation.*` 的写法）。

**第一步（勘察，先读后写）**：读 `module_mov.cc` 现有桩与上游对 MOV 的注释、
确认影片文件格式（`MOV/*.mpg`，注意 RealLive 用的是旧 MPEG-1 变体）与
MediaCodec 的兼容性，再定"解码 → 帧回填 → 呈现"的接口形状。

### v0.2.4 / v0.3.0 计划（DLL 输入模拟 + 小游戏，后移）

打包处理——主线是「让 DLL 驱动的玩法真正能玩」，其余顺带：

| 优先级 | 事项 | 依据 / 现状 |
| --- | --- | --- |
| 高（主线） | **LB 棒球小游戏（`PT00.dll`）** | `systems/base/little_busters_pt00dll.cc` 目前只记录调用并 `return 0`；小游戏默认被 `machine/game_hacks.cc` 的 `LB_SkipBaseball` 跳过（开关 `SetLBSkipBaseball`，由 `rlvm-diag.txt` 的 `lb_minigame=1` 打开） |
| 高（主线） | **DLL 输入模拟**：给 `PT00` 这类要读鼠标/键盘的 DLL 提供输入通道，并预留**可脚本化的模拟输入**以便自动化验证 | 同上 |
| 高（顺带） | **直接杀进程会丢 `global.sav.gz`**：槽位文件已写入，但"槽位已占用"的 global memory 标记与配置改动丢失 → LOAD 列表列不出来。修法：在挂起/退到后台时于引擎线程落盘 | `dev-log/LBEX-SAVE-LOAD.jsonl`；`docs/PROGRESS.md` 已知缺口 |
| 中 | 引擎标准存档 UI 缺失（`Platform::InvokeSyscomStandardUI` 为空） | D-025 |
| 中 | `exec_ignore_force_wait` 是否保留：做一次 A/B | `docs/FRAMERATE-INVESTIGATION.md` 6.5 |
| 低 | T7.3 备份项：面板显示当前文本容器文件名；`docs/PROGRESS.md` 与实际能力对齐 | 本文件 T7.3 |

**取证手段（已就绪，不必先写代码）**：

1. `rlvm-diag.txt` 设 `lb_minigame=1` → 脚本真正进入小游戏，`little_busters_pt00dll.cc`
   的调用记录器把 `func` / 4 个参数 / 场景与行号打进 logcat；
2. 用 `dump_scenario` 反汇编小游戏所在场景，从**脚本怎么用返回值**反推每个 `func` 的语义
   —— 这条路径不依赖原版 `PT00.dll`。

注意取证的一个天花板：记录器只能看到「DLL 返回 0」时脚本走到的分支，某些分支可能因此
永不触发；所以第 2 步（读脚本）是必要的补充，不能只靠调用日志。


### T7.1 点击补齐：未渲染区域也要能推进文字（已完成）

**问题**：`RlvmRenderer.mapToFrame()` 对落在**画面矩形之外**的触摸返回 `null`
（适配模式下左右/上下的黑边就是这种区域），`MainActivity.handleTouch()` 随即把这次触摸
丢掉——点黑边不会推进对白。

**做法（已实现）**

- `mapToFrame()` 里，界外点击统一映射到**渲染画面的顶部中间** `(frameWidth / 2, 0)`：
  RealLive 的推进只看"有没有左键按下"、不看坐标，而顶部中间不会放按钮，因此界外点击既能
  推进文字又不会误触元素；画面内的点击照旧按实际坐标换算；
- 只在"还没有帧"（`frameWidth <= 0`）或"视图尺寸尚未量出"时返回 null；
- 保持既有语义：面板与悬浮球仍由 View 层消费，不转发给引擎。

**已弃用备选**：把界外坐标夹取到最近的画面边缘——会把黑边点击送进贴边按钮。见 D-020。

**验收（真机通过）**：点左右黑边、上下黑边、画面外角落都能推进对白；画面内按钮命中不受
影响；黑边长按仍能唤出右键菜单。

### T7.2 按键通道 + 浮动按键栏（先做出来、默认关闭）（已完成）

**背景**：引擎的按键入口是 `EventListener::KeyStateChanged(KeyCode, pressed)`
（`systems/base/event_listener.h` 里的 `RLKEY_*` 枚举）；SDL 后端在
`systems/sdl/sdl_event_system.cc` 的 `HandleKeyDown/Up` 里 `DispatchEvent(...)`。
**Android 后端目前完全没有按键通道**，所以依赖键盘输入的游戏（如 Little Busters! EX）
拿不到输入。

**做法**

- `AndroidEventSystem`：与 `PostTouchEvent` 并列新增 `PostKeyEvent(int rl_key, bool pressed)`
  与队列，在 `ExecuteEventSystem(machine)` 里 drain，并
  `DispatchEvent(machine, bind(&EventListener::KeyStateChanged, _1, KeyCode(rl_key), pressed))`；
- JNI：`NativeBridge.keyEvent(int rlKeyCode, boolean pressed)`（显式注册，与 `touchEvent` 同规格）；
- UI：面板新增「按键栏」开关（**默认关**，持久化）。开启后显示浮动按键：
  `Ctrl`（按住跳过）、`Space`、`Enter`、`Esc`、方向键、`Z / X / C`；
  默认关闭是刻意的——除 LB 系外的游戏不需要，避免遮挡画面；
- 键位表集中在一处（Kotlin 常量表），日后按 LBEX 实测需要调整只改这张表。

**验收（真机通过）**：开关关闭时行为与现状完全一致；开启后方向键能推动光标、击打能推进
对白、右键能唤出菜单；「加速」注入的键盘事件在日志中可见
（`rlvm-input: key code=304 pressed=1/0`）。见 D-021。

**已知边界**：这条通道**不能**驱动 LB / LBEX 的小游戏——小游戏逻辑在 `PT00.dll` 里，
RLVM 目前用 `LB_SkipBaseball` 直接跳过（`machine/game_hacks.cc`），
`LittleBustersPT00DLL::CallDLL()` 是返回 0 的桩。要让小游戏可玩需另立一项：实现 `PT00`
这个 `RealLiveDLL`（逆向 DLL + 按脚本实际调用的 func 逐个实现）。

### T7.3 备份项（视情况）

- 面板显示「当前文本容器」文件名（便于确认正在读哪一份数据）；
- `docs/PROGRESS.md` 与实际能力对齐（该文档停留在 v0.1.0 时代）。

## 与 task.md 的偏差（已决策）

| 偏差 | 说明 |
| --- | --- |
| compileSdk 用 36 而非 35 | 本机只装了 android-34/36，避免额外下载；`targetSdk` 仍为 35。见 D-002 |
| 音频用 AAudio 而非 Oboe | minSdk 31 > API 26，AAudio 可直接使用。见 D-004 |
| RLVM 源码不复制进 `cpp/rlvm/` | 保留上游目录，由 CMake 引用，便于 `git diff` 审查。见 D-006 |
| 不引入独立 Kotlin Gradle 插件 | AGP 9 内置 Kotlin。见 D-007 |
| T0.2 骨架暂不含 Compose | 保持零第三方依赖以便离线构建；Compose 在 T3.2 引入。见 D-008 |
| T1.2 拆为 1.2a / 1.2b | 先取得可编译基线，再做 SAF 文件系统。见 D-009 |
