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
