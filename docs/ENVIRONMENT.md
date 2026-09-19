# 本机工具链现状（探测 + 验证记录）

> 首次探测：2026-09-19 14:47 +08:00
> 验证更新：2026-09-19 15:10 +08:00
> 背景：用户重装过系统但保留了文件，注册表与 `C:\Program Files` 被重置，原有依赖大概率丢失

## 1. 结论

**工具链已经可用，并且做了端到端验证。** 三项关键能力均已实测通过：

1. `gradle --version` 成功，输出 Gradle 9.1.0 + JVM 21.0.9；
2. NDK 交叉编译成功，arm64-v8a 与 armeabi-v7a 均产出 `.so`，且 `std::filesystem` 可用；
3. git 仓库已建立为当前用户所有，基线提交完成。

仍然存在的两个约束：

- **沙箱内不能写 `~/.gradle` 与 `.git`**，因此 Gradle 构建和 git 提交必须在提权（沙箱外）下执行；
- **Gradle 缓存里没有 `androidx.compose.*`**，而 task.md 要求 Jetpack Compose + Material 3，首次构建需要联网下载。

## 2. 重装系统造成的影响

| 位置 | 是否幸存 | 说明 |
| --- | --- | --- |
| `C:\Program Files\Eclipse Adoptium\jdk-25.0.3.9-hotspot` | **丢失** | 上一次成功构建（2026-07-27）用的就是这个 JDK，Gradle daemon 日志可证 |
| `C:\Users\tooke\.gradle` | 幸存 | 含 760 MB 依赖缓存 + Gradle 9.1.0 发行版 |
| `C:\Users\tooke\.android` | 幸存 | adbkey 等 |
| `E:\DEV\AndroidSdk` | 幸存 | 完整 SDK（E 盘未受影响） |
| `E:\STM32cubeMX\jre` | 幸存 | Temurin 21.0.9 LTS，本次选作 JAVA_HOME |
| 用户环境变量 / PATH | **重置** | 这是"装了却找不到"的根因 |
| `C:\Users\tooke\.gitconfig` | 不存在 | 无全局 git 身份，需仓库级配置 |

## 3. 工具清单（含验证状态）

| 工具 | 状态 | 路径 | 版本 | 实测 |
| --- | --- | --- | --- | --- |
| JDK（**已选定**） | 可用 | `E:\STM32cubeMX\jre` | Temurin 21.0.9+10-LTS | 通过 `java` / `javac -version` |
| JDK（备选） | 可用 | `E:\MCLDownload\ext\jre-v64-220420\jdk17` | Temurin 17.0.2+8 | 仅版本探测 |
| JDK（备选） | 可用 | `E:\MCLDownload\ext\jre-v64-220420\jdk22` | Oracle 22 | 版本偏新，未验证 |
| Android SDK | 可用 | `E:\DEV\AndroidSdk` | — | `sdk.dir` 已被既有工程使用 |
| ├ `platforms` | 可用 | — | android-34、android-36 | 缺 android-35（见第 8 节） |
| ├ `build-tools` | 可用 | — | 28.0.3、34.0.0、36.0.0 | 通过 |
| ├ `ndk` | 可用 | `...\ndk\28.2.13676358` | **r28c**（2.12 GB / 7673 文件，完整） | 交叉编译通过 |
| ├ `platform-tools` | 可用 | `...\platform-tools\adb.exe` | — | 需 `HOME` 才能运行（已设） |
| ├ `cmdline-tools` | 可用 | `...\cmdline-tools\latest\bin\sdkmanager.bat` | latest | 通过 |
| ├ `cmake` | 可用 | `...\cmake\3.22.1` | 3.22.1 | 未实测 |
| └ `licenses` | 已接受 | — | 7 个文件 | 通过 |
| Gradle | 可用 | `~\.gradle\wrapper\dists\gradle-9.1.0-all\...\gradle-9.1.0` | 9.1.0 | `--version` 通过 |
| CMake（独立） | 可用 | `E:\cmake-4.3.2-windows-x86_64\bin\cmake.exe` | 4.3.2 | 未实测 |
| `git` | 可用 | `C:\Program Files\Git\cmd\git.exe` | — | 仓库已建立并提交 |
| `python` / `node` / `winget` | 可用 | 见 PATH | — | 通过 |
| `scons` | 缺失 | — | — | 上游构建系统，本项目改用 CMake，不需要 |
| Android Studio | 未安装 | — | — | 命令行构建不依赖 |

### 3.1 可用于参照的既有工程

`C:\Users\tooke\Desktop\SAKANA\sakana_APK\new_sakana_apk\android`（Flutter 工程的 Android 壳）在本机成功构建过，其配置可作为 T0.2 的已知可用组合参照：

```kotlin
// settings.gradle.kts
id("com.android.application") version "9.0.1" apply false
id("org.jetbrains.kotlin.android") version "2.3.20" apply false
```

```properties
# gradle/wrapper/gradle-wrapper.properties
distributionUrl=https\://services.gradle.org/distributions/gradle-9.1.0-all.zip
# local.properties
sdk.dir=E:\\DEV\\AndroidSdk
```

结论：**Gradle 9.1.0 + AGP 9.0.1 + Kotlin 2.3.20** 是本机验证过的组合，且三者的构件都已在 Gradle 缓存中，可离线解析。注意 AGP 9 是主版本升级，DSL 与 8.x 有破坏性差异（该工程通过 `android.newDsl=false`、`android.builtInKotlin=false` 关闭新特性）。

## 4. 已执行的永久配置（2026-09-19）

用户级（HKCU\Environment）环境变量，已写入并回读确认：

| 变量 | 值 |
| --- | --- |
| `JAVA_HOME` | `E:\STM32cubeMX\jre` |
| `ANDROID_SDK_ROOT` | `E:\DEV\AndroidSdk` |
| `ANDROID_HOME` | `E:\DEV\AndroidSdk` |
| `ANDROID_NDK_HOME` | `E:\DEV\AndroidSdk\ndk\28.2.13676358` |
| `ANDROID_NDK_ROOT` | `E:\DEV\AndroidSdk\ndk\28.2.13676358` |
| `HOME` | `C:\Users\tooke`（修复 adb 的 `Cannot mkdir '\.android'`） |
| `PATH`（追加） | `E:\STM32cubeMX\jre\bin`、`E:\DEV\AndroidSdk\platform-tools`、`E:\DEV\AndroidSdk\cmdline-tools\latest\bin` |

原有用户 PATH 条目全部保留。

**风险提示**：`E:\STM32cubeMX\jre` 是 STM32CubeMX 自带的 JDK 21，路径不带版本号所以相对稳定，但它仍属于第三方软件目录。若将来卸载或大版本升级 STM32CubeMX，`JAVA_HOME` 会失效。建议后续单独安装一份独立的 Temurin LTS 到固定目录（如 `C:\Java\jdk-21`）再改指向。

## 5. 端到端验证结果

### 5.1 Gradle + JDK（提权执行）

```
gradle exit=0
------------------------------------------------------------
Gradle 9.1.0
------------------------------------------------------------
Launcher JVM:  21.0.9 (Eclipse Adoptium 21.0.9+10-LTS)
Daemon JVM:    E:\STM32cubeMX\jre
OS:            Windows 10 10.0 amd64
```

### 5.2 NDK 交叉编译（沙箱内执行）

用 `tools/_smoke/hello.cpp`（含 `<filesystem>`）编译两种 ABI：

| 目标 | 命令要点 | 结果 |
| --- | --- | --- |
| arm64-v8a | `--target=aarch64-linux-android31 -fPIC -shared -std=c++17 -O2` | `hello-arm64.so` (53,728 B) |
| armeabi-v7a | `--target=armv7a-linux-androideabi31 ...` | `hello-armv7.so` (12,780 B) |

ELF 头：`ELF64 / DYN / AArch64`。
LOAD 段对齐：**`0x4000`（16 KB）** —— NDK r28c 默认 16 KB 页对齐，满足 Android 15+ 对 16 KB 页大小的要求。

**关键结论**：`std::filesystem` 在 NDK r28c 的 `-std=c++17` 下可用，架构报告里 R5（`boost::filesystem` 替换）的可行性已确认。

### 5.3 git

```
b87ff44 chore(baseline): RLVM upstream source + T0.1 architecture analysis + toolchain audit
tracked files = 1066
```

仓库已按当前 Windows 账户（而非沙箱账户）重新初始化，避免 `dubious ownership`。已添加 `.gitignore` 与 `.gitattributes`（统一 LF，二进制资源标记）。

## 6. 沙箱限制与提权要求

本会话的沙箱运行在**独立的 Windows 账户** `DESKTOP-6A1HJQR\CodexSandboxOffline` 下，与你的账户 `DESKTOP-6A1HJQR\tooke` 不同。这带来两个必须提权才能完成的动作：

| 动作 | 沙箱内表现 | 处理方式 |
| --- | --- | --- |
| Gradle 构建 | `Could not initialize native services. Failed to load native library 'native-platform.dll'`（无法在 `~/.gradle/native` 建锁文件） | 用提权执行；已为 gradle.bat 申请前缀规则 |
| git 写操作 | `could not lock config file .git/config: Permission denied` / `Unable to create .git/index.lock` | 用提权执行 |
| 写用户环境变量 | 工作区外只读 | 已完成（提权执行） |
| 读操作 / 沙箱内编译 | 正常 | 无需提权 |

注意：沙箱内创建的文件属主是 `CodexSandboxOffline`，提权执行的命令以 `tooke` 运行。这曾导致 git 的 `dubious ownership` 报错，已通过"以 tooke 身份重建仓库"解决。

## 7. 仍然缺失 / 需要联网的部分

| 项 | 现状 | 影响 |
| --- | --- | --- |
| `androidx.compose.*`（含 material3、runtime、ui） | **Gradle 缓存中没有** | task.md 要求 Compose + Material 3，首次同步必须联网 |
| `platforms;android-35` | 未安装 | 见第 8 节 |
| AGP 9.0.1 / Kotlin 2.3.20 及传递依赖 | 已在缓存 | 可离线解析 |
| Oboe | 未获取 | **不需要**：minSdk 31 > API 26，直接用 AAudio |
| Boost（若保留 Boost.Serialization） | 未获取 | 存档格式兼容需要；NDK 交叉编译 Boost 需联网下载源码 |

## 8. 对 task.md 计划的影响

1. **T0.2 验收前提已具备**，但首次 `assembleDebug` 需要联网下载 Compose 相关依赖与 AGP/Kotlin 插件（若缓存命中不全）。
2. **SDK 版本取舍（已完成决策）**：采用 `minSdk = 31`、`targetSdk = 35`、`compileSdk = 36`、`buildToolsVersion = 36.0.0`。理由：本机只有 android-34 与 android-36，用已安装的 36 可避免额外下载；`compileSdk` 与 `targetSdk` 在 AGP 中本就允许不同，`targetSdk = 35` 保持 task.md 对行为契约的要求，`minSdk = 31` 不变。若后续需要 android-35，可用 `sdkmanager` 补装。
3. **T4.1 去掉 Oboe 依赖**：直接使用 AAudio（API 26+，minSdk 31 已满足），减少一个第三方库和一次网络下载。
4. **构建与提交需要提权**：这是沙箱策略决定的常态，不是一次性问题。后续每个阶段的构建验证与 git 提交都会申请提权。
