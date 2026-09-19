# 决策记录（ADR 简版）

格式：编号 / 日期 / 决策 / 理由 / 状态

---

## D-001 用户级环境变量指向现有 JDK 21 与 Android SDK

- **日期**：2026-09-19
- **决策**：`JAVA_HOME=E:\STM32cubeMX\jre`（Temurin 21.0.9 LTS）、`ANDROID_SDK_ROOT=E:\DEV\AndroidSdk`，并追加 JDK/bin、platform-tools、cmdline-tools 到用户 PATH。
- **理由**：本机重装系统后注册表被重置，但 E 盘工具幸存。除 STM32CubeMX 自带 JDK 21 外，其余 JDK 均来自 Minecraft 启动器（17 / 22），路径带日期目录更易失效；21 是现存最新 LTS。
- **遗留风险**：该 JDK 属于第三方软件目录，卸载 STM32CubeMX 会使 `JAVA_HOME` 失效。建议后续换装独立 Temurin LTS。
- **状态**：已执行

---

## D-002 采用 compileSdk 36 + targetSdk 35 + minSdk 31

- **日期**：2026-09-19
- **决策**：`compileSdk = 36`、`targetSdk = 35`、`minSdk = 31`、`buildToolsVersion = 36.0.0`。
- **理由**：本机 SDK 只装了 android-34 与 android-36。`compileSdk` 只影响用哪套平台 API 编译，与运行时行为契约（`targetSdk`）可以不同；用已安装的 36 可避免额外下载并保持面向新 API 的向前兼容，`targetSdk` 仍按 task.md 保持 35。
- **备选**：`sdkmanager "platforms;android-35"` 补装后改用 `compileSdk = 35`（需要联网）。
- **状态**：已决定，待 T0.2 落地

---

## D-003 采用 Gradle 9.1.0 + AGP 9.0.1 + Kotlin 2.3.20

- **日期**：2026-09-19
- **决策**：以本机已缓存的组合作为构建基线。
- **理由**：该组合在 `C:\Users\tooke\Desktop\SAKANA\sakana_APK\new_sakana_apk\android` 上成功构建过；Gradle 9.1.0 发行版是 `~/.gradle/wrapper/dists` 中唯一已下载的发行版，AGP 9.0.1 与 Kotlin 2.3.20 构件也都在缓存中，可离线解析。
- **备注**：AGP 9 为主版本升级，DSL 与 8.x 存在破坏性差异；新工程按 9.x DSL 编写，不使用已删除的配置项。
- **状态**：已决定，待 T0.2 落地

---

## D-004 音频层直接用 AAudio，不引入 Oboe

- **日期**：2026-09-19
- **决策**：`systems/android` 的音频后端直接调用 AAudio。
- **理由**：task.md 的 `minSdk = 31` 已高于 AAudio 的引入版本（API 26），Oboe 只是 AAudio / OpenSL ES 之上的便捷封装。去掉它可减少一个第三方依赖、一次联网下载和一层封装。
- **状态**：已决定，待阶段 4 落地

---

## D-005 git 仓库以当前用户身份初始化

- **日期**：2026-09-19
- **决策**：删除沙箱账户创建的 `.git` 后，以 `tooke` 身份重建并提交基线。
- **理由**：沙箱运行在独立的 Windows 账户下，其创建的 `.git` 属主与当前用户不一致，git 会以 `dubious ownership` 拒绝所有操作。
- **备选**：保留原 `.git` 并配置 `safe.directory`（更绕，且后续写入仍受沙箱只读限制）。
- **状态**：已执行（提交 `b87ff44`，1066 个文件）

---

## D-006 RLVM 上游源码保留在原目录，不复制进 app/src/main/cpp/

- **日期**：2026-09-19
- **决策**：上游源码继续放在仓库根的 `rlvm-release-0.14/`，由 CMake 以 `RLVM_SOURCE_DIR` 变量引用；不按 task.md 建议结构复制成 `app/src/main/cpp/rlvm/`。
- **理由**：① 避免 14 MB 源码重复，杜绝两处副本不同步；② 修改平台层时 `git diff rlvm-release-0.14/` 能精确反映对上游的改动，符合"最小修改 + 可审查"的要求。
- **代价**：CMake 里会出现跨目录的相对路径引用，需要用一个变量集中管理。
- **状态**：已决定，待 T1.1 落地

---

## D-007 采用 AGP 9 原生配置（新 DSL + 内置 Kotlin）

- **日期**：2026-09-19
- **决策**：不使用参照工程（Flutter 模板）里的 `android.newDsl=false` / `android.builtInKotlin=false`，也不声明 `org.jetbrains.kotlin.android` 插件；改用 AGP 9 的默认新 DSL 与内置 Kotlin 支持。
- **理由**：首次构建时 AGP 9.0.1 明确报警告：这两个开关已废弃、默认值分别为 `true`，且将在 AGP 10 移除；`org.jetbrains.kotlin.android` 在 AGP 9 起不再需要。沿用废弃路径会把技术债带到整个移植周期。
- **验证**：迁移后 `.\gradlew.bat clean assembleDebug` 通过（1m43s），`--warning-mode=all` 下无任何废弃告警。
- **备注**：参照工程之所以用旧配置，是因为 Flutter 模板尚未跟进 AGP 9；不能直接照抄。
- **状态**：已执行并验证

---

## D-008 T0.2 骨架不引入 Compose，推迟到 T3.2

- **日期**：2026-09-19
- **决策**：T0.2 的骨架只依赖 Gradle 自带能力（纯 `android.app.Activity` + 程序化视图），不引入任何 androidx / Compose 依赖。
- **理由**：① 本机 Gradle 缓存中完全没有 `androidx.compose.*`，引入即必须联网；② 阶段 1、2 的工作集中在 C++/JNI/文件系统，Compose 在这两个阶段毫无用处，保持零依赖可以让 `assembleDebug` 全程离线可跑，编译迭代更快更稳；③ 首个真实 Compose 需求出现在 T3.2（游戏库与设置 UI）。
- **备选**：T0.2 即引入 Compose（需联网下载约数十 MB 依赖）。
- **状态**：已执行

---

## D-009 T1.2 拆分为 T1.2a（std::filesystem）与 T1.2b（桌面依赖替换）

- **日期**：2026-09-19
- **决策**：把 task.md 中"用 Oboe 替代 SDL 音频、NDK filesystem 替代 Boost.Filesystem"一条拆成两步。
- **理由**：先把 `boost::filesystem` 换成 `std::filesystem` 获得**可编译基线**（此时仍假定能用普通路径访问），SAF 语义留到 T2.2；否则"编译适配"与"文件访问模型重写"两个不同性质的问题会互相干扰，难以定位错误。
- **依据**：`tools/_smoke/hello.cpp` 已实测 NDK r28c 下 `std::filesystem` 可用（见 `docs/ENVIRONMENT.md` 5.2）。
- **状态**：已决定
