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
| T1.4 | 最小 JNI 验证 | `native-bridge.cpp` | App 能调用 native 并返回结果 | 部分完成：RegisterNatives 桥接与 App 内调用已就位，待接入真实引擎实例 |

## 阶段 2：文件系统与 JNI 桥接

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T2.1 | JNI 接口设计 | `NativeBridge.kt`、`native-bridge.cpp` | 方法可被调用，无动态查找 | TODO（骨架已建立 RegisterNatives 模式） |
| T2.2 | `VirtualFileSystem` / AndroidFileSystem（SAF） | `android-fs.cpp/.h` | 能读取 `SEEN.txt` | TODO |
| T2.3 | 触摸事件映射 | Kotlin 触摸监听 + JNI | 游戏内光标可移动、点击 | TODO |
| T2.4 | 引擎生命周期 | JNI + Kotlin 封装 | 能启动、暂停、恢复、退出 | TODO |

## 阶段 3：Compose UI 与 SAF

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T3.1 | SAF 目录选择器 | `DirectoryPicker.kt` | 重启后仍可访问目录 | TODO |
| T3.2 | 游戏库与设置 UI（**在此引入 Compose + Material 3**） | `MainScreen.kt`、`GameViewModel.kt` | 界面可交互 | TODO |
| T3.3 | GLSurfaceView 集成 | `GameScreen.kt` | 游戏画面能显示 | TODO |

## 阶段 4：音频与渲染

| ID | 任务 | 输出 | 验收 | 状态 |
| --- | --- | --- | --- | --- |
| T4.1 | AAudio 音频（原计划用 Oboe，已改，见 D-004） | `audio-engine.cpp` | 语音播放正常，无爆音 | TODO |
| T4.2 | 渲染管线重写（OpenGL 1.x 固定管线 → GLES 3.0） | `gl-renderer.cpp`、`GameRenderer.kt` | 画面正常，无花屏 | TODO |
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

## 与 task.md 的偏差（已决策）

| 偏差 | 说明 |
| --- | --- |
| compileSdk 用 36 而非 35 | 本机只装了 android-34/36，避免额外下载；`targetSdk` 仍为 35。见 D-002 |
| 音频用 AAudio 而非 Oboe | minSdk 31 > API 26，AAudio 可直接使用。见 D-004 |
| RLVM 源码不复制进 `cpp/rlvm/` | 保留上游目录，由 CMake 引用，便于 `git diff` 审查。见 D-006 |
| 不引入独立 Kotlin Gradle 插件 | AGP 9 内置 Kotlin。见 D-007 |
| T0.2 骨架暂不含 Compose | 保持零第三方依赖以便离线构建；Compose 在 T3.2 引入。见 D-008 |
| T1.2 拆为 1.2a / 1.2b | 先取得可编译基线，再做 SAF 文件系统。见 D-009 |
