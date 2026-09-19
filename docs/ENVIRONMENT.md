# 本机工具链现状（探测记录）

> 探测时间：2026-09-19 14:47 +08:00，全程只读，未安装、未修改任何环境
> 目的：确认 T0.2 及后续阶段能否在本机完成构建验收

## 1. 结论

**本机具备构建能力，但开箱即用不成立**——JDK、Android SDK、NDK、Gradle、CMake 全部不在 PATH 上，且工作区不是 git 仓库。需要先做一次环境变量配置和少量组件补装。

已就位：Android SDK（`E:\DEV\AndroidSdk`）、NDK r28c、三套可用 JDK、Gradle 9.1.0 发行版缓存、SDK 自带 CMake 3.22.1、已接受的 SDK licenses。
缺失：PATH 上的 JDK、`android-35` platform、git 仓库、可用的网络（沙箱默认受限）。

## 2. 工具清单

| 工具 | 状态 | 路径 | 版本 | 备注 |
| --- | --- | --- | --- | --- |
| `java` / `javac` | 可执行，不在 PATH | `E:\MCLDownload\ext\jre-v64-220420\jdk17` | Temurin 17.0.2+8 | **推荐**，AGP 8.x 要求 JDK 17 |
| 备选 JDK | 可执行 | `E:\STM32cubeMX\jre` | Temurin 21.0.9 LTS | STM32CubeMX 自带，AGP 8.5+ 可用 |
| 备选 JDK | 可执行 | `E:\MCLDownload\ext\jre-v64-220420\jdk22` | Oracle 22 | 版本偏新，AGP 兼容性存疑 |
| Android SDK | 已安装 | `E:\DEV\AndroidSdk` | — | 见下方子项 |
| ├ `platforms` | 已安装 | `E:\DEV\AndroidSdk\platforms` | android-34、android-36 | **缺 android-35** |
| ├ `build-tools` | 已安装 | `E:\DEV\AndroidSdk\build-tools` | 28.0.3、34.0.0、36.0.0 | 可用 34/36 |
| ├ `ndk` | 已安装 | `E:\DEV\AndroidSdk\ndk\28.2.13676358` | **r28c** | 满足 task.md 的 r26+ |
| ├ `platform-tools` | 已安装 | `E:\DEV\AndroidSdk\platform-tools\adb.exe` | — | 沙箱下需设 HOME 才能运行 |
| ├ `cmdline-tools` | 已安装 | `E:\DEV\AndroidSdk\cmdline-tools\latest\bin\sdkmanager.bat` | latest | 可补装缺失组件 |
| ├ `cmake` | 已安装 | `E:\DEV\AndroidSdk\cmake\3.22.1` | 3.22.1 | AGP 外部原生构建默认版本 |
| └ `licenses` | 已接受 | `E:\DEV\AndroidSdk\licenses` | 7 个 license 文件 | 含 android-sdk-license |
| Gradle | 发行版已缓存 | `C:\Users\tooke\.gradle\wrapper\dists\gradle-9.1.0-all` | 9.1.0 | 首次 `assembleDebug` 仍需下载 AGP 等插件 |
| CMake（独立） | 已安装 | `E:\cmake-4.3.2-windows-x86_64\bin\cmake.exe` | 4.3.2 | 可用于手工验证；NDK 构建建议走 SDK 版 |
| `git` | 在 PATH | `C:\Program Files\Git\cmd\git.exe` | — | 工作区**不是** git 仓库 |
| `python` / `pip` | 在 PATH | `...\Programs\Python\Python314` | 3.14 | |
| `node` / `npm` / `npx` | 在 PATH | `...\Programs\nodejs` | — | |
| `winget` | 在 PATH | `...\WindowsApps\winget.exe` | — | 可用于安装缺失组件 |
| `scons` | **缺失** | — | — | 上游构建系统；本项目改用 CMake，不影响 |
| Android Studio | **未安装** | — | — | 命令行构建不依赖它 |
| `7z` / `unzip` | 缺失 | — | — | 解压 Boost/Oboe 源码包时可能需要 |

## 3. 发现的问题及影响

| 编号 | 问题 | 影响 | 建议方案 |
| --- | --- | --- | --- |
| E1 | JDK / `ANDROID_HOME` / `ANDROID_SDK_ROOT` 均未设置，PATH 上无 `java` | 任何 `gradlew` 调用立即失败，T0.2 无法验收 | 方案 A（推荐，零副作用）：每次调用前在命令内显式设置环境变量；方案 B：写入用户级环境变量（需你批准） |
| E2 | `adb.exe` 在沙箱内报 `Cannot mkdir '\.android': Permission denied` | 无法用 adb 安装/调试 APK | 显式设置 `HOME` / `USERPROFILE` / `ANDROID_SDK_HOME` 后重试 |
| E3 | SDK 只有 android-34 / android-36，**没有 android-35** | `compileSdk = 35` 无法直接解析 | 三选一：① `sdkmanager "platforms;android-35"`（需网络）；② `compileSdk = 36` + `targetSdk = 35`；③ 降级到 compileSdk 34 |
| E4 | 工作区不是 git 仓库 | task.md 工作流中的「Git commit」步骤无处落地，也无法用 diff 做变更审查 | 需要 `git init`（等你确认）或从远端 clone |
| E5 | 沙箱网络默认受限 | 首次 Gradle 同步（AGP/Kotlin 插件）、Oboe/Boost 等依赖下载会失败 | 需要时我会用 `require_escalated` 单独申请，不会静默重试 |
| E6 | PowerShell 把原生命令的 stderr 当错误记录（`NativeCommandError`） | `java -version`、`git` 等正常输出会被标红，容易误判为失败 | 读取时统一加 `2>&1` 并对 `$LASTEXITCODE` 判成败，不看颜色 |

## 4. 建议的环境变量设置（未执行，待批准）

```powershell
$env:JAVA_HOME         = 'E:\MCLDownload\ext\jre-v64-220420\jdk17'
$env:ANDROID_SDK_ROOT  = 'E:\DEV\AndroidSdk'
$env:ANDROID_HOME      = 'E:\DEV\AndroidSdk'
$env:ANDROID_NDK_HOME  = 'E:\DEV\AndroidSdk\ndk\28.2.13676358'
$env:HOME              = $env:USERPROFILE
$env:PATH              = "$env:JAVA_HOME\bin;$env:ANDROID_SDK_ROOT\platform-tools;$env:ANDROID_SDK_ROOT\cmdline-tools\latest\bin;$env:PATH"
```

## 5. 对 task.md 计划的影响

1. **T0.2 验收条件需补充前提**：`./gradlew assembleDebug` 通过的前提是上述环境变量生效，且首次同步能联网下载 AGP/Kotlin 插件。
2. **T4.1 的 Oboe 依赖可降级**：`minSdk = 31` 已经高于 AAudio 的引入版本（API 26），AAudio 可直接使用。Oboe 只是 AAudio/OpenSL ES 之上的便捷封装，不是必需依赖。建议直接调用 AAudio，省掉一个第三方库与一次网络下载；若后续需要更低的延迟调优再引入 Oboe。
3. **`android-35` 的取舍需要与 T0.2 一起定**：`compileSdk` 与 `targetSdk` 在 AGP 中本就可以不同，`compileSdk = 36` + `targetSdk = 35` 是合法组合，可避免额外下载。
4. **E4 的 git 仓库**：建议在 T0.2 之前先 `git init`，这样每个任务的 diff 都能被审查，也符合 task.md 的工作流与「人工检查点」设计。
