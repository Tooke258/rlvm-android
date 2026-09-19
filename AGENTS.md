# AGENTS.md — rlvm-android 项目级指令

> 本文是 Agent 每次启动后的第一读物。建议阅读顺序：
> `AGENTS.md` → **`docs/PROGRESS.md`（进度交接，先读这个）** → `TASKS.md` →
> `dev-log/` 最近日志 → `docs/DECISIONS.md` → `docs/ARCHITECTURE.md`

## 1. 项目目标

把 RLVM（VisualArt's RealLive 虚拟机，GPLv3）的 C++ 核心封装为现代 Android 应用。

| 项 | 值 |
| --- | --- |
| 包名 / namespace | `org.rlvm.android` |
| minSdk / targetSdk / compileSdk | 31 / 35 / 36 |
| buildToolsVersion | 36.0.0 |
| ABI | `arm64-v8a`、`armeabi-v7a` |
| 原生工具链 | NDK r28c（`28.2.13676358`）、CMake 3.22.1、C++17 |
| 构建工具 | Gradle 9.1.0 + AGP 9.0.1（内置 Kotlin，不单独引入 Kotlin 插件） |
| UI | Jetpack Compose + Material 3（在 T3.2 引入，见 D-008） |
| 音频 | AAudio（不使用 Oboe，见 D-004） |
| 渲染 | OpenGL ES 3.0 + GLSurfaceView |
| 文件访问 | Storage Access Framework（SAF），禁止直接 File 路径 |
| 许可 | GPLv3，项目必须开源 |

## 2. 目录结构

```
rlvm-release-0.14/                     # 工程根 = git 仓库根
├── AGENTS.md  TASKS.md  task.md
├── docs/            ARCHITECTURE.md（源码架构分析）
│                    ENVIRONMENT.md（工具链现状与验证）
│                    DECISIONS.md（决策记录 D-00N）
├── dev-log/         <task-id>.jsonl 任务日志
├── tools/_smoke/    NDK 交叉编译冒烟测试
│                    setup_third_party.ps1 获取第三方源码
├── third_party/     第三方源码（gitignore；Boost/ogg/vorbis，跑脚本生成）
├── app/             Android 应用模块
│   ├── build.gradle.kts
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/org/rlvm/android/
│       └── cpp/     CMakeLists.txt、native-bridge.cpp
└── rlvm-release-0.14/   RLVM 上游源码（只读基线，见 D-006）
```

## 3. 硬性规则

1. **先读后写**：改任何文件前，先读相关源码、CMake、Gradle、Manifest 和错误日志。
2. **最小修改**：只改必要文件，不重构 RLVM 核心逻辑。
3. **每改必验**：每次修改后运行对应构建命令，并把结果写入 `dev-log/`。
4. **显式 JNI**：所有 native 方法必须用 `RegisterNatives` 显式注册，不得依赖按符号名的动态查找。
5. **SAF 优先**：Android 12+ 外部文件访问必须走 SAF 与 `DocumentFile`。

### 禁止事项

- 不下载、不打包任何商业游戏资源；
- 不使用 `READ_EXTERNAL_STORAGE` 等宽泛存储权限；
- 不在音频回调中分配内存、做 I/O、加锁、休眠；
- 不引入 GPLv3 不兼容的专有库；
- 不修改 `libreallive`、`machine`、`modules`、`encodings` 的脚本解析与指令语义。

## 4. 构建与验证

构建所需环境变量已配置为**永久用户环境变量**（见 `docs/ENVIRONMENT.md` 第 4 节）：

```
JAVA_HOME          = <JDK 21 路径>
ANDROID_SDK_ROOT   = <Android SDK 路径>
ANDROID_NDK_HOME   = <Android SDK 路径>\ndk\28.2.13676358
```

本机各占位符对应的真实路径见 `local-data/LOCAL-PATHS.md`（该目录不进仓库）。

常用命令（在工程根执行）：

```powershell
.\gradlew.bat assembleDebug            # T0.2 起的验收命令
.\gradlew.bat clean assembleDebug      # 干净重建
```

### 沙箱注意事项（重要）

本机 Codex 沙箱运行在独立 Windows 账户下，`~/.gradle` 与 `.git` 对沙箱只读，因此：

- **Gradle 构建必须提权执行**，否则报 `Could not initialize native services. Failed to load native library 'native-platform.dll'`；
- **git 写操作必须提权执行**，否则报 `could not lock config file .git/config: Permission denied`；
- 读取、搜索、以及直接调用 NDK 的 `clang++.exe` 可以在沙箱内完成。

NDK 目录里 clang 的可执行文件是 `clang++.exe` / `clang.exe`（不是 `.cmd`）。

## 5. 修改 RLVM 上游代码的规则

- 上游源码保留在 `rlvm-release-0.14/` 原处，**不复制**进 `app/src/main/cpp/`（见 D-006），由 CMake 以变量形式引用。
- 只允许改平台相关部分；`libreallive` / `machine` / `modules` / `encodings` 的语义不得改动。
- 每处改动都要能用 `git diff rlvm-release-0.14/` 单独审查。

## 6. 记录约定

| 内容 | 位置 |
| --- | --- |
| 任务日志 | `dev-log/<task-id>.jsonl`（JSONL，一行一条） |
| 技术决策 | `docs/DECISIONS.md`，编号 `D-00N` |
| 源码架构 | `docs/ARCHITECTURE.md` |
| 环境与工具链 | `docs/ENVIRONMENT.md` |
| 真机测试回路 | `docs/TESTING.md`（设备、adb 命令、测试夹具） |

## 7. 停止与升级条件

- 连续 3 次修同一编译错误仍失败 → 暂停，请人工介入；
- 涉及 RLVM 核心逻辑改动 → 暂停，等待用户确认；
- 涉及 Manifest 权限变更、GPL 合规判断 → 暂停，等待用户确认；
- 每个阶段结束 → 人工检查点。
