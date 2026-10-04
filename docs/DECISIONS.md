# 决策记录（ADR 简版）

格式：编号 / 日期 / 决策 / 理由 / 状态

---

## D-001 用户级环境变量指向现有 JDK 21 与 Android SDK

- **日期**：2026-09-19
- **决策**：`JAVA_HOME` 指向本机现存的 Temurin 21.0.9 LTS，`ANDROID_SDK_ROOT` 指向本机 Android SDK，并追加 JDK/bin、platform-tools、cmdline-tools 到用户 PATH。
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
- **理由**：该组合在本机另一个工程的 Android 壳上成功构建过；Gradle 9.1.0 发行版是 `~/.gradle/wrapper/dists` 中唯一已下载的发行版，AGP 9.0.1 与 Kotlin 2.3.20 构件也都在缓存中，可离线解析。
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
- **决策**：删除沙箱账户创建的 `.git` 后，以本机账户身份重建并提交基线。
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

## D-018 触摸输入：UI 线程入队、引擎线程注入，按下与抬起分帧

- **日期**：2026-09-19
- **决策**：`MainActivity` 在 `GLSurfaceView` 上收触摸事件，用
  `RlvmRenderer.mapToFrame()` 把视图坐标换成游戏帧坐标（与画面缩放共用同一套
  fit 规则，避免坐标对不上），经 `NativeBridge.touchEvent` 投递；
  native 侧只做入队（`AndroidEventSystem::PostTouchEvent`），真正的注入在引擎线程的
  `ExecuteEventSystem` 里完成。另加 `touch_button` 诊断开关（位掩码 1=左键 2=右键
  3=两者，缺省 1）。
- **理由**：注入必须广播给 `EventListener`（按钮对象、选择肢），只能在引擎线程做；
  UI 线程也不能被引擎工作量阻塞，所以中间隔一层队列。
- **关键细节（踩坑）**：**同一帧内「按下+抬起」必须拆到两帧**。第一版把 DOWN/UP 在
  一次 drain 里连续应用，脚本只看到 `button == 2`（已松开），标题菜单因此毫无反应；
  拆帧后（抬起推迟到下一次 `ExecuteEventSystem`）左键单击立刻生效。
  这正是「坐标对了、悬停高亮也对，但点不动」的原因。
- **语义**：与上游 SDL 后端一致——按下 `button = 1`，抬起 `button = 2`，
  `FlushClick` 清零；`MouseButtonStateChanged` 的派发沿用 SDL 的写法。
- **验证**：真机点标题菜单 START → 悬停高亮（START 变橙色）+ 光标画在触点，
  单击后加载正文资源 `ss_mw00a/b/c/d/e`（`ss_mw00c` 有 128 个子图）并进入正文，
  底部出现游戏内 HUD（Auto/Skip/Q.Save/Q.Load/Config/Close）。
  左键单独与左右同按结果一致，因此缺省用左键。
- **状态**：已执行并验证

---

## D-017 自建 44.1k→48k 重采样（`ResamplingSource`）

- **日期**：2026-09-19
- **决策**：在 `audio_engine` 里新增 `ResamplingSource` 装饰器，`OpenSource()`
  在 `MakeConverter` **之前**读走解码器的真实采样率，速率不同就套一层线性插值重采样
  到 `WAVFILE::freq`（48000）。不改上游、不改 vendored xclannad。
- **理由**：`WAVFILE::MakeConverter` 里 `SDL_BuildAudioCVT(cvt, from_format, ch, freq,
  format, 2, freq)` 把**源速率**也传成了目标速率 48k，SDL 因此认为不需要转换；
  xclannad 自带的 `conv_wave_rate` 又只在 `freq < original->SamplingRate`（即目标低于
  源）时才生效。两条路都不覆盖「44.1k → 48k」，于是数据被当成 48k 直接播：
  **快 8.8%、音调高约 1.5 个半音**（用户报「音调偏高，高音耳机略有爆音感」）。
- **为什么不用更好的插值**：16-bit 游戏 BGM 用线性插值已足够，且不会引入过冲；
  换窗口化 sinc 只是质量优化，不是正确性问题，留待需要时再评估。
- **副作用**：解码线程多一层 memcpy 与一次浮点乘加；音频回调完全不受影响
  （重采样在解码线程上，回调只是从环形缓冲取数）。
- **验证**：`audio_selftest=1` 时运行自检——合成 440Hz / 44100Hz 正弦，
  直通（模拟修复前）测得 **478.367Hz**，经重采样后测得 **439.509Hz**（期望 440），
  1 秒输入产出 47999 帧（期望 48000）。真机上重采样后 BGM 连续三个窗口
  `active_channels=1`、峰值非零，未被饿死。
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

---

## D-010 Boost 1.92.0 从源码编译所需库，绕开 b2

- **日期**：2026-09-19
- **决策**：使用 Boost 1.92.0 源码树（b2-nodocs 布局），在 CMake 中直接编译 Boost.Filesystem / Boost.Serialization / Boost.Iostreams 的源文件。
- **理由**：① RLVM 核心 149 个文件中完全不含 `<SDL/...>`，但深度依赖 Boost——`BOOST_CLASS_VERSION` ×9、`boost::serialization::access` ×24、文本归档 ×72，存档格式无法用标准库替代；② 实测核心用到的 36 个 Boost 头里只有 1 个缺失（`filesystem/convenience.hpp`，早已被移除，其 API 已在 `operations.hpp`），说明 1.92 几乎可直接使用；③ 直接编源码比把 b2 调到能交叉编译更可控。
- **代价**：Boost 源码树需由 `tools/setup_third_party.ps1` 获取（约 140 MB，gitignore）。CMake 用 `BOOST_SOURCE_DIR` 定位，优先级为 `-DBOOST_SOURCE_DIR=` > 环境变量 > gitignore 的 `local.properties` 里的 `boost.dir=` > `third_party/` 下的解压目录。
- **状态**：已执行并验证

---

## D-011 定义 BOOST_FILESYSTEM_SINGLE_THREADED，保持 C++17

- **日期**：2026-09-19
- **决策**：全局定义 `BOOST_FILESYSTEM_SINGLE_THREADED`，`CMAKE_CXX_STANDARD` 保持 17。
- **理由**：Boost 1.92 的 `libs/filesystem/src/atomic_tools.hpp` 直接使用 `std::atomic_ref`（C++20），而 **NDK r28c 的 libc++ 根本没有实现该类**——实测确认 `std::atomic_ref` 在 libc++ 头文件中不存在，因此升到 C++20 也无效。Boost 为此提供官方单线程开关：定义后内部改用非原子访问。该宏只出现在 Boost.Filesystem 的两个**私有源文件**中，公共头文件不引用，故不构成 ODR 风险。
- **语义影响**：Boost.Filesystem 的全局 path locale 指针与目录迭代内部状态不再原子访问。RLVM 的文件操作集中在引擎线程，风险低。
- **遗留**：若将来确认不需要兼容既有 PC 存档，可迁移到 `std::filesystem` 并彻底移除该开关与本库（见 T1.2a）。
- **状态**：已执行并验证（两 ABI 均通过）

---

## D-012 用最小 SDL 兼容层顶替 wavfile.cc 的音频转换依赖

- **日期**：2026-09-19
- **决策**：新增 `app/src/main/cpp/compat/sdl_shim/SDL/SDL_mixer.h` 与其实现，提供 `SDL_AudioCVT` / `SDL_BuildAudioCVT` / `SDL_ConvertAudio` 及所需常量，**不改动 vendored 源码**。
- **理由**：`vendor/xclannad/wavfile.cc` 是核心文件列表中唯一引用 `<SDL/SDL_mixer.h>` 的文件，但它实际只用 SDL 做 PCM 格式转换（S8↔S16）、声道数转换与线性重采样，并未使用 SDL 的窗口/事件/混音功能。该文件被 `nwk_voice_archive.cc` 依赖，无法简单排除。
- **备选**：改写 vendored 文件（diff 更大、偏离上游更多）；或在阶段 4 让 AAudio 后端直接接受源格式（更彻底，但依赖更大的重构）。
- **清理条件**：T4.1 的 AAudio 后端能够直接接受源格式后，可移除本 shim。
- **状态**：已执行并验证

---

## D-013 libogg / libvorbis 从源码编入，只编解码器

- **日期**：2026-09-19
- **决策**：从 GitHub 获取 libogg 1.3.5 与 libvorbis 1.3.7，在 CMake 中作为静态库编译；libvorbis 只编译解码所需源文件。
- **理由**：OVK 与 KOE 语音包是 Ogg Vorbis，`ovk_voice_sample.cc` / `ovk_voice_archive.cc` / `koedec_ogg.cc` 使用 `ov_open_callbacks` / `ov_info` / `ov_read` 接口。编码器部分（`vorbisenc.c` / `psytune.c` / `analysis.c` / `barkmel.c` / `tone.c`）依赖 `lib/modes/*.h` 这类构建期生成的头文件，而 RLVM 不需要编码功能。注意 `psy.c` 必须保留，`res0.c` 等仍会引用其 `_vp_*` 符号。
- **下载源**：xiph 官方源在本机 302 响应耗时 129 秒，GitHub 归档仅 4 秒，故脚本优先 GitHub。
- **许可**：libogg / libvorbis 为 BSD 许可，与 GPLv3 兼容。
- **状态**：已执行并验证

---

## D-014 在 fd 层面把 stdout / stderr 重定向到 logcat

- **日期**：2026-09-19
- **决策**：新增 `android/log_redirect.{h,cc}`，在 `JNI_OnLoad` 里用管道接管 fd 1 / fd 2，
  由读取线程逐行转发给 `__android_log_print`（标签 `rlvm-stdout` / `rlvm-stderr`）。
- **理由**：Android 应用的原生标准输出被丢进 `/dev/null`，而上游 RLVM 的诊断信息
  （未实现的操作码、被吞掉的指令异常、模块警告，共 200 余处 `cout` / `cerr` / `printf`）
  全部走这两个流。结果就是「跑了上千条指令却什么都看不到」，任何基于日志的判断都无法进行。
- **备选**：替换 `std::streambuf`（只覆盖 C++ 流，漏掉 `printf` 与直接 `write(1, ...)`）；
  或输出到文件（需要额外拿路径，且不如 logcat 便于 `adb logcat` 直接过滤）。
- **副作用**：管道有 64 KB 缓冲，极端情况下若读取线程卡住会让写端阻塞；读取线程只做
  转发，实测无影响。逐行输出还顺带绕开了 logcat 单条消息的长度上限。
- **状态**：已执行并验证（正是它让「首屏全黑」的根因在几分钟内定位）

---

## D-015 在 AndroidSurface 上实现 GRP type-2 区域表（GetPattern）

- **日期**：2026-09-19
- **决策**：`AndroidSurface` 保存 `region_table_`，加载图像时把 xclannad 解码器的
  `region_table`（每个子图的矩形 + 原点偏移）搬进来，并覆写
  `GetNumPatterns()` / `GetPattern(int)`；无区域表的资源退化为「整张图一个子图」。
- **理由**：上游对象渲染的源矩形完全来自 `GraphicsObjectData::SrcRect()`，它取的是
  `CurrentSurface(go)->GetPattern(go.GetPattNo()).rect`。基类的默认实现返回静态空
  `GrpRect`，于是**每个对象的源矩形都是 0×0**——对象存在、也确实参与渲染循环，
  但一个像素都画不出来。真机表现是「脚本跑了几万条指令、合成统计里上万次 blit，
  画面却 100% 全黑」。
- **证据链**：见 `docs/PROGRESS.md` 第 5 节。关键证据是上游自带的
  `GraphicsSystem::Refresh(std::ostream*)` 转储，它直接打印出
  `Rendering Rect(0, 0, Size(0, 0)) to Rect(0, 0, Size(0, 0))`。
- **语义影响**：只影响 Android 后端的 `Surface` 实现，未改动 `libreallive` / `machine` /
  `modules` / `systems/base` 的任何语义。
- **状态**：已执行并验证（Kud Wafter 标题画面完整呈现，见 PROGRESS 第 3 节）

---

## D-016 应用默认不限时运行，并提供停止与防重入

- **日期**：2026-09-19
- **决策**：`time_budget_ms` 缺省改为 `0`（不限时）；`MainActivity` 的指令上限放到
  `Int.MAX_VALUE`；新增「停止引擎」按钮 → `NativeBridge.requestStop()` → native 置
  `g_stop_requested`，引擎线程在下一轮循环开头收尾；同时用 `RunGuard` 保证任一时刻
  只有一台引擎在运行。
- **理由**：探针阶段「跑 3 秒就返回报告」是为了诊断，但真机表现是**画面和 BGM 只能
  存在几秒**——一停止循环，引擎对象析构，游戏音乐自然就断了。应用要能一直停在标题/
  正文上，就必须让引擎持续运行；而持续运行又必须解决「怎么停」和「重复点运行会起
  第二台引擎（两台共用 AudioEngine 与帧缓冲）」这两个问题。
- **代价**：报告的停止原因只有在停止或达到指令上限时才打印。自动化验证需要把
  `time_budget_ms` 写进 `rlvm-diag.txt`（见 docs/TESTING.md）。
- **验证**：不设诊断文件运行 72 秒仍在跑且 `active_channels=1`（BGM 持续）；
  点「停止引擎」后报告 `stop reason = stop requested`；重复点击返回
  `ERROR: 已有一台引擎在运行，请先点「停止引擎」。`
- **状态**：已执行并验证

---

## D-017 屏幕熄灭 / 进后台时挂起引擎（而不是继续跑）

- **日期**：2026-10-04
- **决策**：新增 `NativeBridge.setEngineSuspended(boolean)`。`MainActivity` 在
  `onStop`（进后台）与 `ACTION_SCREEN_OFF/ON` 广播（MIUI 这类"Activity 仍然 started
  但屏幕已熄灭"的厂商差异）两处上报状态；native 侧只置一个进程级标志
  `g_engine_suspended`，引擎循环在每轮开头看到它就跳过 `system.Run()`（不合成帧）与
  字节码执行、让出 20 ms 睡眠；同时 `AudioEngine::SetSuspended()` 让混音输出静音
  **并暂停 AAudio 数据回调**（`AAudioStream_requestPause/requestStart`）。
  恢复后从原位置继续——不退出、不重载、不丢进度。
- **理由**：此前只有 `onPause` 停掉 GL 线程，引擎线程依然按 10 ms 时间片推进字节码、
  音频回调依然出声，用户观察到的现象是「黑屏之后引擎仍然正常运行」。挂起（而不是
  停止）是为了让唤醒后能原地继续；只静音不停回调则不够——AAudio 每秒仍会唤醒 CPU
  几百次，黑屏期间白白耗电（真机日志里能看到音频 HAL 在挂起后
  `needStopPlaybackTask force:1`）。
- **实现要点**：
  - 挂起标志是**进程级**的（Kotlin 侧放在 `companion object`）：native 那台引擎活在
    进程里，而 Activity 可能被系统回收后重建；标志若跟着 Activity 走，重建出来的实例
    会以为"没挂起"，从而永远不去通知 native 恢复。
  - 挂起不消耗 `time_budget_ms` 之外的东西：默认 `time_budget_ms=0`（不限时），
    因此不影响既有行为。
- **验证**（Redmi K40 / 天玑1200 / Android 12，release 包）：
  `input keyevent 26` 熄屏后日志出现
  `rlvm-audio: suspended=yes` + `rlvm-native: engine suspended=yes`，随后 11.5 s
  内**零帧推进**（`progress:` 与 `frame` 日志完全停止）；`input keyevent 224`
  唤醒后出现 `suspended=no`，帧计数与 BGM 立即恢复
  （`rlvm-audio: runtime: active_channels=1 peak_in_window=4384`）。
- **语义影响**：只动 Android 后端（`native-bridge.cpp` / `audio_engine.*` /
  `MainActivity.kt`），未触碰 `libreallive` / `machine` / `modules` 的任何语义。
- **状态**：已执行并验证

---

## D-018 文本容器（语言）显式开关：不改用户的原始 SEEN.TXT

- **日期**：2026-10-04
- **背景**：汉化数据是一份**单独的容器文件**（日文场景 + 中文场景合并后的 Seen 归档，
  见 `local-data/tools-l10n/merge_container.py`，产物只留本机）。不能要求用户去覆盖游戏
  根目录的 `SEEN.TXT`——那是用户的原始文件，覆盖了就切不回日文。
- **决策**：面板上加「选择文本容器」+「容器：开/关」两个入口：
  - 列表由**应用自己**列游戏目录树的候选文件（`Seen*` / `*.txt` / `*.bin` / `*.dat` /
    `*.exe`，带大小），用 `AlertDialog` 选；**不使用** `ACTION_OPEN_DOCUMENT`——本机 MIUI
    在选择大文件时会把系统文件界面搞崩（实测两次，均为系统 UI 进程被拖死）；
  - 选择结果按**游戏目录树内的相对路径**持久化（`Seen-CN.TXT`），
    `NativeBridge.setTextContainerPath()` → native 保存进程级路径 →
    `RunScenarioSaf` 用 `SafBackend::OpenFd()` 打开；打不开则回退到 `Seen.txt` 并打印 WARNING；
  - 游戏目录里的 `Seen####.txt` 覆盖文件仍然**优先于**容器（补丁机制的既有语义）。
- **验证**：日志可见 `text container = Seen-CN.TXT` / `(game dir Seen.txt)` 来回切换；
  中文容器 `TOC entries=63`（原版 62）且游戏内文本为中文；切回原版即日文原文。
- **语义影响**：只动应用层（Kotlin + native-bridge），未改 `libreallive` 语义。
- **状态**：已执行并验证

---

## D-019 字节码扫描按场景编码 + 「零前进」守卫（本次唯一的核心改动）

- **日期**：2026-10-04
- **背景（真机事故）**：接入中文容器后，引擎在解析场景 2720 时**内存涨到 RSS 5.27GB +
  swap 4.65GB**，被系统 `lowmemorykiller` 以 `oom_score_adj 0` 杀掉；LMK 顺手清掉一批
  后台进程，用户看到的是「资源管理器崩了」。
- **根因**：`libreallive/bytecode.cc` 的 `BuildFunctionElement()`（以及 `NextData` 内部、
  运行时 `GetData`/`GetComplexParam`、`GosubWithElement` 共 5 处）长这样：
  `while (*p != ')') { size_t n = NextData(p); params.push_back(string(p, n)); p += n; }`
  —— `NextData()` 对「既不认识的 Shift_JIS 首字节、也不在允许单字节集合里」的字节**返回 0**，
  于是 `p` 不前进、每轮还往 vector 里 push 一个元素 → 死循环 + 无限增长。
  汉化补丁把参数里的引号/逗号/换行改写过，GBK 文本里就出现这种字节（中文引导字节多在
  0xB0-0xF7，而上游只认到 0x9F）。
- **决策（用户批准的两层改动）**：
  1. **L2 编码感知扫描**：新增 `CurrentTextEncoding()/SetCurrentTextEncoding()/IsTextLeadByte()`
     （`libreallive/expression.h/.cc`）。`IsTextLeadByte()` 在 CP932 下与上游**逐字节一致**
     （0x81-0x9F、0xE0-0xEF），在 CP936/CP949 下按 0x81-0xFE 判定；6 处写死的判定全部改用它。
     编码在两个时点设置：`Script` 构造（按场景声明的编码解析元素树，`scenario.cc`）与
     `ExecuteNextInstruction`（每条指令前，跟随当前场景，`rlmachine.cc`）。
  2. **L1 零前进守卫**：上述 5 处循环里，若某一步没有推进，立即抛 `libreallive::Error`
     （报错信息里带卡住位置附近的 16 字节十六进制，便于定位）。合法 token 一定消耗 ≥1 字节，
     所以这条只可能对坏数据触发——效果是「该场景解析失败」，而不是吃几 GB 内存。
  另加两道保险：`Script` 解析时限制元素数（≤ 解压长度/4），以及运行时内存探针
  （`mem[...] rss=` 日志 + 1.2GB 自停）。
- **验证（Redmi K40 / 天玑1200 / Android 12，release 包）**：
  - 事故前：只解析到场景 2719 就无输出，33s 后 `lowmemorykiller: Kill 'org.rlvm.android'
    ... to free 5523896kB rss, 4650160kb swap`；
  - 改动后：63 个场景全部解析，`mem[after export] rss≈245MB`、运行期稳定在 ~260MB，
    LMK 零记录；逐场景元素数均为几十~几千（无异常膨胀）；帧率不受影响（引擎合成 ~340fps，
    显示侧 `fps=120` 满帧、单帧 ≤9.5ms）；
  - 守卫按设计只拦下 **1 个场景：2728**（`BuildFunctionElement(): parameter list makes no
    progress at 00 0a 7b 04 …`）。**该场景已定位并修好**（见下），最终 63/63 全部解析成功
    （`export_jp_text: scenes=63 strings=33289 failed=0`）。
- **2728 的根因（数据缺陷，不是格式差异）**：把中文版与日文原版按「行号标记」对齐后可见——
  ```
  JP: 0a 77 04 | 29 | 0a 78 04 0a 79 04 0a 7a 04 | 0a 7b 04 | 40 70 01 キャンキャン、と…
  CN: 0a 77 04 | 00 | 0a 7b 04 | 40 70 01 "还伴随着小小的叫声。"
  ```
  即补丁的转换器把**外层参数列表的闭括号 `)` 写成了 NUL**（并丢掉三段空行标记）。RLVM 扫参数
  列表时永远等不到 `)`，一路扫到那个 NUL → `NextData()` 返回 0 → 零前进。
  扫全部 31 个中文场景，「行标记 + NUL」这种形态还有 3 处（2720/9611/9619），但都不在未闭合的
  参数列表里，解析正常——所以这是孤例级的数据写坏。
  修法：`local-data/tools-l10n/fix_name_brackets.py` 的 `SCENE_BYTE_FIXES` 做**等长替换**
  （2728 解压流偏移 74244：`00` → `29`），偏移完全不变（kidoku 表 / 入口点无需调整），
  并带上下文断言（前文必须 `0a 77 04`、后文必须 `0a 7b 04`）。
- **语义影响**：`libreallive/expression.cc`、`libreallive/bytecode.cc`、`libreallive/scenario.cc`、
  `machine/rlmachine.cc` —— CP932 数据的行为逐字节不变（默认编码 0 走原判定），
  GBK 数据从「误判/死循环」变成「正确切分或干净失败」。
- **状态**：已执行并验证（63/63 场景全中文，含 2728）

---

## D-020 界外点击映射到画面顶部中间（v0.2.1 T7.1）

- **日期**：2026-10-04
- **背景**：适配模式下画面两侧/上下有黑边。`RlvmRenderer.mapToFrame()` 原先对画面矩形
  **之外**的触摸直接返回 `null`，`MainActivity.handleTouch()` 随即丢弃这次触摸——于是
  点黑边（以及任何没渲染到的地方）完全没有反应，用户想靠"点空白推进对白"时会点空。
- **决策**：界外点击统一映射到**渲染画面的顶部中间** `(frameWidth / 2, 0)`；画面**之内**
  的点击照旧按实际坐标换算。理由：RealLive 的对白推进只看"有没有左键按下"、不看坐标；
  而画面顶部中间不会放界面按钮，所以界外点击既能推进文字，又不会误触任何元素。
- **已弃用的备选**：把界外坐标"夹取到最近的画面边缘"——会把黑边点击送进贴边的按钮
  （例如标题界面贴着边缘的选项），风险更大。
- **实现**：`app/src/main/java/org/rlvm/android/RlvmRenderer.kt::mapToFrame()`；
  只在"还没有帧"或"视图尺寸尚未量出"时返回 `null`。长按＝右键、滑动＝移动鼠标的语义不变；
  控制面板与悬浮球仍由 View 层消费，不转发给引擎。
- **验证**：真机（Kud Wafter）点左右黑边、上下黑边、画面外角落都能推进对白；画面内按钮
  的命中不受影响；黑边长按仍能唤出右键菜单。
- **语义影响**：只动 Android 前端的坐标换算，未触碰引擎语义。
- **状态**：已执行并验证（T7.1 闭环）

---

## D-021 浮动按键栏 + 按键事件通道（v0.2.1 T7.2）

- **日期**：2026-10-04
- **背景**：RealLive 的按键入口是 `EventListener::KeyStateChanged(KeyCode, pressed)`
  （`systems/base/event_listener.h` 的 `RLKEY_*`），SDL 后端在 `sdl_event_system.cc` 的
  `HandleKeyDown/Up` 里 `DispatchEvent(...)`；**Android 后端此前完全没有按键通道**，
  同时 `ShiftPressed()`/`CtrlPressed()` 是写死的 `false`。
- **决策**：分两层补齐，且**默认关闭**：
  1. **通道**：`AndroidEventSystem::PostKeyEvent(rl_key, pressed)` 与触摸并列入队，在
     `ExecuteEventSystem()` 里注入 `KeyStateChanged`；`ShiftPressed()/CtrlPressed()`
     改为如实回答（Ctrl 是"按住跳过"的判定来源）。JNI：`NativeBridge.keyEvent(code, pressed)`。
  2. **界面**：面板新增「按键栏：开/关」（默认关、持久化）。开启后显示半透明浮层：
     - 四向方向键（按住持续）→ **移动鼠标光标**（每 60ms 一步 8px）。理由：RealLive 脚本
       今天读得到的"方向"只有鼠标位置，键盘对脚本不可见；
     - 「加速」（按住）→ 键盘键（默认 `LSHIFT`）；
     - 「击打」（按住）→ 鼠标左键按下/抬起；
     - 「右键」（按住）→ 鼠标右键按下/抬起（与长按等价）。
     除按钮本身外不拦截触摸，未命中的事件照旧落到游戏画面。
- **已知边界（重要）**：**这条通道今天不能驱动 LB/LBEX 的小游戏**。原因是小游戏逻辑在
  `PT00.dll` 里，而 RLVM 对 `LB.ENV`/`LB_EX.ENV` 有专门的 hack
  （`machine/game_hacks.cc`: `AddLineAction(7030, 15, LB_SkipBaseball)`）直接把那次
  farcall 返回掉；`LittleBustersPT00DLL::CallDLL()` 也只是一个返回 0 的桩。要让小游戏能玩，
  必须先实现 `PT00` 这个 `RealLiveDLL`（见 §"下一步"）。本决策只负责把入口铺好：
  方向、鼠标左右键、键盘键三条路都已验证可用，DLL 补上后不需要再改输入层。
- **验证**（真机 Redmi K40 / Android 12）：开关关闭时行为与之前完全一致；开启后方向键能
  推动光标、击打能推进对白、右键能唤出游戏菜单；「加速」在游戏里无可见效果（符合预期），
  但日志里能看到 `rlvm-input: key code=304 pressed=1/0` 注入到引擎。
- **语义影响**：只动 Android 后端（`android_system.*` / `native-bridge.cpp` / Kotlin UI），
  未改 `libreallive` / `machine` 语义。
- **状态**：已执行并验证

---

## D-022 合帧改为脏标记驱动 + 合成快路径（性能）

- **背景**：主循环此前每轮无条件 `Refresh()`：全屏背景 + 所有对象 + 文字的重合成
  白吃时间（实测每轮 ~16ms，其中 blit 占 11ms+）。
- **决策**：
  1. **合帧闸门**：与上游 `SDLGraphicsSystem::ExecuteGraphicsSystem` 一致，只在
     `screen_needs_refresh()` 时合成一帧，合成后 `OnScreenRefreshed()` 清标记。
     诊断开关 `dirty_gate=0` 可退回旧行为做 A/B。
  2. **整面不透明快路径**：`AndroidSurface` 维护 `pixels_opaque_`（整面 alpha 是否
     全 255，图片加载时与包围盒共用一次扫描算出），命中时 `BlitToSurface` 走整行
     `memcpy`。关键是**目标侧状态要正确传播**：`DC0`/帧缓冲都是靠 blit 写入的，
     若无条件失效该标志，背景层永远拿不到快路径。
  3. **`CaptureFrame` 只在内容哈希变化时调用**：否则 GL 线程每轮重传 1.9MB 纹理。
  4. **包围盒裁剪**：源图层"整屏大小、内容只占一角"时把源矩形裁到保守包围盒。
- **验证**：静止场景 `loop dt` 6.2ms（`run` 0.02ms）；动画场景合帧从 16ms 降到
  11ms/轮。**注意：这些优化没有改变"动画台阶"症状**（内容变化率仍是 8.5/s），
  说明瓶颈不在合成——见 D-023。
- **语义影响**：只动 Android 后端渲染层，未改 `libreallive` / `machine` 语义。
- **状态**：已执行；动画台阶问题另见 D-023

---

## D-023 动画台阶感：根因是计时基点（绝对时钟 vs 相对 tick），已修复并真机验证

- **真正根因（2026-10-04 定位并验证）**：Android 后端的 `NowMillis()`
  （`app/src/main/cpp/android/android_system.cpp`）返回的是 `steady_clock` 的
  **绝对**毫秒（自开机起算，量级 **10⁹**），而 RLVM 的帧计数器把 tick 存进
  **`float`**（`systems/base/frame_counter.h` 的 `float time_at_last_check_`）。
  IEEE-754 单精度只有 24 位有效位：在 `[2³⁰, 2³¹)` 区间 ULP = `2⁷ = **128ms**`。
  于是 `ms_elapsed` 只能取 0 或 128 → 计数器每次推进
  `128 / 0.1 = **1280**`（正好是全量程 10000 的 12.8%）→ 每秒只推进 `1000/128 ≈ **8 次**`
  → alpha 每级跳 32/255 → **8 级台阶**。
  - PC 原生没有此问题，因为 `SDL_GetTicks()` 返回的是「自 `SDL_Init` 起的毫秒数」
    （10⁵~10⁶ 量级），float 在该量级的分辨率是微秒级。
  - **严重程度随设备开机时长变化**（ULP = 2^(指数−23)）：开机 <12.4 天时 ULP=64ms
    （约 15 级），12.4–24.8 天时 ULP=128ms（8 级）。这解释了该症状为何"稳定又不能复现"。
- **修复**：`NowMillis()` 改成**相对进程起点**的毫秒（对齐 `SDL_GetTicks()` 语义），
  只改平台后端，未触碰任何指令语义。
- **真机验证（Redmi K40 游戏版 / LBEX，2026-10-04）**：

  | 指标 | 修复前 | 修复后 |
  | --- | --- | --- |
  | `FrameCounter` 值推进次数 | 5 / 8 次每秒 | 451 / 522 / 588 次每秒 |
  | 内容变化率（`content changed`） | 8.3 /s | 92 ~ 123 /s |
  | alpha 每级步长 | 32 / 255 | 3 ~ 4 / 255 |
  | 合帧 vs 内容变化 | 124.7/s 合帧、仅 8.3/s 有变化（94% 重复劳动） | 121.7/s 合帧 = 121.7/s 变化（1:1） |

  用户真机确认：开屏 logo/淡变**已平滑**。
- **对早期结论的更正**：下面那条 `force_wait` 因果链**是误判**——8 级台阶与
  `force_wait` 无关（修复计时基点后，即使仍忽略 `force_wait` 也已平滑）。
  `exec_ignore_force_wait` 目前仍为默认 true；它是否还有必要，建议用
  `rlvm-diag.txt` 的 `exec_ignore_force_wait=0` 做一次 A/B 后再定（见
  `docs/FRAMERATE-INVESTIGATION.md` 第 6.3 节）。
- **上游脆弱性提示（不改上游）**：`FrameCounter` 用 `float` 存 tick 本身是隐患，
  任何"返回绝对大数值时钟"的后端都会踩到。我们在后端守住 `GetTicks()` 的契约
  （小数值、单调、相对起点）即可，并在 `NowMillis()` 里留了注释防止回退。

### 早期排查记录（已被上面的根因取代）

- **症状**：开屏实时渲染动画（警告淡变、Key logo）有明显阶梯感；PC 端平滑。
- **已确认的因果链**（完整证据、数据、复现方法见
  [`docs/FRAMERATE-INVESTIGATION.md`](FRAMERATE-INVESTIGATION.md)）：
  1. 脚本 `SEEN9011` 用 `InitFrame(counter, 0, 10000, 1000)` 要求 **10000 级 /
     1000ms** 的淡变（每级 0.1ms，本该完全平滑）；
  2. 插值公式（`index_series` → `Interpolate`）与 `change_interval` 计算**都正确**；
  3. 但脚本每圈要 **128ms** 才转完，于是 1000ms 只跑出 **8 级台阶**；
  4. 原因是脚本每圈调用的 `refresh()` 指令 = `GraphicsSystem::ForceRefresh()`，
     它在 `SCREENUPDATEMODE_MANUAL` 下执行 `system().set_force_wait(true)`，
     而主循环 exec 的退出条件含 `!system.force_wait()` → **每轮提前结束**。
- **当时的决策（已被上面根因取代）**：主循环忽略 `force_wait`
  （`exec_ignore_force_wait`，默认 true）。
- **状态**：**根因已定位、已修复、已真机验证**（见本文档开头）。
- **附带确认**：`MOV/op00.mpg` 开场影片因 `module_mov.cc` 全为 `AddUnsupportedOpcode`
  而不被播放——这是**独立的功能缺口**，与台阶问题无关。
- **语义影响**：只动 Android 后端主循环；上游 `ForceRefresh`/`force_wait` 的语义
  本身未改。

---

## D-024 无平台 GUI 时，存档回退到游戏自带脚本路径

- **背景**：LBEX 的 `GAMEEXE.INI` 是 `SYSTEMCALL_SAVE_MOD=0` +
  `SYSTEMCALL_SAVE=9999,10`。按上游逻辑，`save_mod != 1` 时
  `System::InvokeSaveOrLoad`（`systems/base/system.cc`）会走
  `platform_->InvokeSyscomStandardUI()`——也就是 GCNPlatform 的 Guichan 存档对话框。
  本移植**从未调用 `SetPlatform`**（`platform_` 为 null），于是点击存档**静默无操作**。
- **决策**：`save_mod != 1` 且**没有 platform** 时，回退到游戏自带的脚本路径
  （`SYSTEMCALL_SAVE` 指向的场景 + 入口点）。桌面版有 platform，行为完全不变。
- **真机验证（2026-10-04，Redmi K40 游戏版 / LBEX）**：
  - **写入**：`/…/files/.rlvm/KEY_リトルバスターズ！ＥＸ/save000.sav.gz` 正常生成
    （1390 B，zlib 流，解压后 35724 B，`boost` 文本档头 + CP932 标题 + 时间戳齐全）。
  - **持久化**：**重启 APP 后文件仍在**，`global.sav.gz` 一并保留。
  - **读取**：重启后引擎枚举槽位时，`SaveExists(slot=0)` 与 `SaveInfo(slot=0)`
    都返回 1（游戏存档菜单的实际调用点，见下）。**LOAD 界面能列出该槽位**，
    「重启后 LOAD 列表为空」的现象不再复现。
  - 结论：**写入侧、持久化侧、读取/列表侧三处都通了**。
- **机制定位（2026-10-04，基于整库 351 个场景的反汇编）**：LBEX 的存档/读档**完全不经过
  `SYSCOM`**，因此不经过 `System::InvokeSaveOrLoad`。实证链：
  1. 右键菜单走 `System::ShowSyscomMenu`：`CANCELCALL_MOD=1` → `farcall(9020, 0)`
     （即游戏自带的菜单场景，不是引擎标准菜单）。
  2. `SEEN9020` 自身不含存档逻辑，靠 `farcall` 拉出各子界面：`9109`（菜单项动画/分页）、
     **`9023`（存档/读档界面）**、`8260`（另一个子界面）。
  3. `SEEN9023` 全部用引擎指令完成存/读：
     - 列槽位：`SaveInfo(槽号, …)`（1413，内部再调 `SaveExists`(1409)）；
     - 存档：`save_always(槽号)`（3107）；
     - 读档：`load_always(槽号)`（3109）。
  4. 全库检索：**没有任何场景调用** `menu_save`(3000) / `menu_load`(3001) /
     `menu_save_always`(3100) / `menu_load_always`(3101)；而上游 `System::InvokeSyscom`
     **只有** `InvokeSyscomAsOp` 一个调用者（`general_operations.cc:138`）。

  ⇒ **`System::InvokeSaveOrLoad` 对 LBEX 不可达，本条决策引入的脚本回退是死代码。**
  之前「改了才存得下来」的观察属于**归因错误**——真正生效的是早已提交的读钩子
  （`utilities/file.cc` 的 `GameFileExists` / `ReadGameFileAll`）加上游戏自带脚本；
  另外 `SEEN9999` 在 LBEX 里根本不存在（TOC 最小索引 513），
  `SYSTEMCALL_SAVE/LOAD=9999,10/11` 是**悬空场景号**，Farcall 只会抛异常。
- **建议**：把 `systems/base/system.cc` 这处回退**还原成上游写法**，保持「上游基线可审、
  不留未经证实的行为改动」。将来若遇到确实依赖引擎标准存档 UI 的作品，再带证据加回来。
- **顺带确认的引擎缺口（LBEX 实际用到、RLVM 未实现）**——整库反汇编统计：

  | 指令 | 出现的场景 | 影响 |
  | --- | --- | --- |
  | Sys 2055 / 2056 | 9010（启动）、9024（config 菜单） | 一对 0/1 设置：2005 是 getter（9024 读回）、2055 是 setter。未实现 ⇒ config 里该项永远读回默认值 |
  | Sys 2005 | 9024 | 同上（getter） |
  | Sys 300 / 1231 / 3503 | 9012 / 9013（引擎标准菜单场景）、9517 / 8731 / 8754 / 8757 | 标准 syscom 菜单链路用到 |
  | EventLoop 1203 / 1204 / 1205 | 9020 / 9109 | 菜单场景用到（1200–1202 已实现：文字窗 override 的 show/hide/clear） |
  | `InvokeSyscom(9)` | 9024 | = `SYSCOM_FONT_SELECTION`，本移植无 platform ⇒ 静默无操作 |
- **新增诊断工具（长期保留）**：`rlvm-diag.txt` 支持
  `dump_scenario=<场景号>[,<场景号>…]` 与 `dump_scenario=all`，把指定/全部场景
  反汇编成 RLVM 源码形式写到应用外部文件目录，用来逆向系统脚本。见
  `app/src/main/cpp/native-bridge.cpp` 的 `DumpScenarioToFile()` /
  `DumpAllScenariosToFile()`。默认关闭，不写诊断文件时行为与以前一致。
- **语义影响**：只改 `systems/base/system.cc` 的平台适配层与 Android 侧诊断，
  未改脚本语义。
- **状态**：存档/读档链路**已验证可用**；本条回退**已验证对 LBEX 不可达**，
  已于 2026-10-04 **还原为上游写法**（`git status` 对 `systems/base/system.cc` 为空）。
  通用性边界与接入条件见 **D-025**。

---

## D-025 存档链路：通用性边界与接入条件

- **结论**：本移植的存档/读档**不含任何作品特判**，对新作品"接入即可用"的
  **前提是游戏自带存档界面脚本**（绝大多数 RealLive 作品都是这样）。
- **两层结构**：
  1. **指令层（RLVM 自带，未改）**：`SaveExists(1409)` / `SaveDate(1410)` /
     `SaveTime(1411)` / `SaveDateTime(1412)` / `SaveInfo(1413)` / `GetSaveFlag(1414)` /
     `LatestSave(1421)` / `save(3007)` / `save_always(3107)` / `load(3009)` /
     `load_always(3109)`，见 `src/modules/module_sys_save.cc`。
     文件名为 `save%03d.sav.gz`，目录为 `System::GameSaveDirectory()`
     = `$HOME/.rlvm/<REGNAME>`（`REGNAME` 取自 `Gameexe.ini`，**天然按作品隔离**）。
  2. **平台 I/O 层（本移植）**：`src/utilities/file.cc` 的
     `WriteGameFile` / `ReadGameFileAll` / `GameFileExists`，配 `native-bridge.cpp`
     的两个 fd 钩子——**绝对路径走 posix**（存档目录位于应用外部文件目录，是真实路径），
     **相对路径走 SAF**（游戏资源）。全部由路径/标识驱动，**无作品分支**。
- **接入条件**：
  - **必需**：作品脚本自己实现存档/读档界面。LBEX（`SEEN9010` → `9020` → `9023`，
    用 `SaveInfo` + `save_always` + `load_always`）与 Kud Wafter 已验证。
  - **当前缺口**：若作品把存/读**交给引擎标准界面**（`SYSTEMCALL_*_MOD=0` 且脚本自己
    不画界面），本移植**无法工作**——因为移植里**没有任何 `Platform` 实现**
    （`SetPlatform` 从未被调用，`Platform::InvokeSyscomStandardUI` 是空的）。
    这正是上游 GCNPlatform/Guichan 承担的部分；要补需另行立项。
- **验证口径（写入 / 固化 / 读取）**：
  - **写入**：`save000.sav.gz` 生成，且 zlib 解压后是完整的 boost 文本档（含
    `CURRENT_LOCAL_VERSION`、`SaveGameHeader`、local memory、machine system 等）。
  - **固化**：重启 App 后文件仍在（`global.sav.gz` 同样保留）。
  - **读取**：引擎枚举槽位时 `SaveExists`/`SaveInfo` 返回 1，且游戏内 LOAD 界面列出该槽位。
  LBEX 三项均已真机实测通过（见 `dev-log/LBEX-SAVE-LOAD.jsonl`）。
- **已归档的误判**：见 D-024。
- **状态**：通用链路已确认；唯一缺口是"引擎标准存档 UI"（未实现）。

---

## D-026 v0.2.2：代码回滚到 v0.2.0 + 计时基点修复（黑闪是耦合回归，未定位）

- **背景**：v0.2.1 之后真机发现**过场时整屏黑闪**（Kud Wafter 与 LBEX 都有；v0.2.0 实测没有）。
  注意是"一段黑"而不是"一帧黑"：探针实测黑场持续 **200ms ~ 3s**，且前几帧往往已在变暗
  （`464111 → 455863 → 324140 → 0`），说明游戏**有意清屏**（`SEEN9030` 里就是
  `op<1:030:00000,0>()` = `stackClear()` + 清对象 + 重新加载），黑场时长被我们的
  资源加载时间放大了。
- **排查过程（都做了单独 A/B，**全部失败**）**：

  | 单独关掉的变量 | 开关 | 结果 |
  | --- | --- | --- |
  | D-022 blit 优化（内容包围盒裁剪 + 不透明 memcpy） | `blit_fast=0` | 还闪 |
  | 脏标记闸门（每轮无条件合帧） | `dirty_gate=0` | 还闪 |
  | `exec_ignore_force_wait`（恢复 refresh 的时间片语义） | `exec_ignore_force_wait=0` | 还闪 |
  | 计时基点（退回 v0.2.0 的绝对时钟） | `tick_absolute=1` | 还闪 |
  | GL 呈现路径（整份 `RlvmRenderer.kt` 退回 v0.2.0） | — | 还闪 |
  | `CaptureFrame` 哈希闸门（每轮都送帧） | `capture_all=1` | 还闪 |

  **每一个单独排除都无效**，因此判断为**耦合回归**（多个改动叠加才出现），继续二分性价比低。
- **决策**：**代码整体回滚到 v0.2.0，只保留计时基点修复**（`NowMillis()` 用相对进程起点
  —— 唯一被数据证明的真根因修复：内容变化 8.3 → 92~123 次/秒，alpha 每级 32 → 3~4）。
  真机验收：**平滑 + 无黑闪**。
- **代价（本次回滚一并去掉，后续要按需重加并逐个验收）**：
  T7.1 黑边点击推进对白、T7.2 按键通道与浮动按键栏、D-022 合帧优化、
  `loop_probe` / `dump_scenario` / `blackframe_probe` 等诊断工具。
  **LBEX 这类依赖键盘输入的作品在 v0.2.2 里拿不到输入**——重加 T7.2 时应优先。
- **保留物**：本地 `git stash@{0}` 保存了本次用到的全部诊断开关实现
  （`blit_fast` / `blackframe_probe` / `capture_all` / `tick_absolute`），
  以及 `docs/FRAMERATE-INVESTIGATION.md`、`dev-log/` 的全部排查记录。
- **后续如果重加功能**：建议一个功能一提交、每步真机验收黑闪——
  "是谁把黑闪带回来的"会自己浮出来，且风险最小。
- **状态**：v0.2.2 基线已验证（平滑、无黑闪）；黑闪的耦合原因**未定位**，已记录在案。

### D-026 补充：黑闪根因已定位并修复（同日）

回滚后的**增量回归**（每加一个功能验一次黑闪）把元凶逼了出来：

| 步骤 | 内容 | 黑闪 |
| --- | --- | --- |
| 1 | 重加 T7.2（按键通道 + 浮动按键栏） | 无 |
| 2 | 重加 T7.1（界外点击映射） | 无 |
| 3 | 重加 D-022（脏标记闸门 + blit 快路径） | **出现** |
| 3.1 | 定点 A/B：`dirty_gate=0`（关闸门、留快路径） | **消失** |
| 3.1 | 定点 A/B：`blit_fast=0`（关快路径、留闸门） | 仍闪 → 像素路径无关 |

- **根因**：`AndroidGraphicsSystem::BeginFrame()` 每帧先把帧缓冲**填成不透明黑**，再由
  `DrawFrame()` 把 DC0 blit 回去；而 **D-022 的脏标记闸门**使合帧只在被标脏时发生。
  若某次合帧正落在「DC0 已清空、内容尚未画上」的瞬间（过场、加载立绘时很常见），
  这张黑帧会**一直显示到下一次被标脏**——实测整屏黑 200ms~3s。v0.2.0 每轮都合帧，
  同样的空状态下一轮（~6ms）就被正确内容覆盖，因此不可见。
- **修复**：`g_dirty_gate` 与 `DiagOptions::dirty_gate` **默认值都改为 false**
  （每轮无条件合帧，恢复 v0.2.0 的合成时机），**保留** D-022 的另一半
  （blit 不透明快路径 + 内容包围盒）——单次合帧成本仍显著低于 v0.2.0。
  ⚠️ 踩坑记录：只改 `g_dirty_gate` 的全局默认是不够的，因为每次启动都会用
  `DiagOptions::dirty_gate` 覆盖它；**两处必须同时改**。凡带默认值的开关，
  都应顺带核对启动报告里的实际生效值。
- **归因上限（诚实记录）**：v0.2.1 里还有第二个独立成因（`CaptureFrame` 的内容哈希闸门，
  或呈现侧的帧号提前返回——两者都在回滚中一并去掉了，未再逐一验证）。
  因此本次只回滚了**闸门**、保留了**快路径**；将来若要重新开启"跳过重复合帧"一类优化，
  必须连带验证黑闪。
- **最终状态**：平滑 ✓、无黑闪 ✓、输入（按键 + 界外点击）✓、合帧走 blit 快路径 ✓，
  真机确认**性能优于 v0.2.0 与 v0.2.1**。

## D-027 MOV 影片通路（v0.2.3 / M3a）：自写 MPEG-PS 解复用 + 平台解码器，根因在 PES 包头

**日期**：2026-10-04　**状态**：M3a 验证通过

**背景**：`MOV/*.mpg`（LBEX 与 Kud Wafter 都是 **MPEG-1 程序流**，800x600@29.97，90~223MB）
在 RLVM 里 7 条 `mov*` 指令全是 `AddUnsupportedOpcode`，这一路要新做。

**结论（按证据，逐条都可复现）**

1. **平台解复用器用不了**：本机只有注册扩展名 `m2p m2ts mts ts` 的 `MPEG2-PS/TS Extractor`，
   `AMediaExtractor_setDataSource(path)` 对 `.mpg` / 改名后的 `.m2p` 一律返回 `-10002`(UNSUPPORTED)
   → **必须自己解 PS**。
   附带坑：路径式 `setDataSource` 是让**解复用服务进程**去开文件，App 私有目录
   `Android/data/<pkg>/files` 别的 uid 读不到，会同样报 `-10002`；正解是**自己开 fd 再
   `AMediaExtractor_setDataSourceFd`**（将来接 SAF 也是这个姿势，SAF 给的正是 fd）。
2. **解码器没问题**：MTK `c2.mtk.mpeg2.decoder` 能正确解 MPEG-1。用 ffmpeg 把同一份影片
   转封成 TS，再加 MP4 / 合成 MPEG-1 / 合成 MPEG-2 三个控件，全部走平台通路解码正确。
3. **根因是自写 PS 解复用的 PES 包头少跳 1 字节。**
   - 视频 PES 载荷结构：`[0xFF 填充]<61 39>[PTS/DTS?]<ES>`，头长由第 3 字节高 4 位定：
     `0x2 → +5（PTS）`、`0x3 → +10（PTS+DTS）`、**其它（本片恒为 `0x0F`）→ +1（标记字节）**。
   - 原来写成 `0x00 → +0`，于是**每个无 PTS/DTS 的包都往 ES 里多塞 1 字节**
     （KW：39,458 个包 = ES 多出 39,458 字节），单 slice 图片从错位处开始崩，
     解码器只写出图片上半部分（Y 平面 68% 是 0）→ 症状就是"残帧 / 下半屏黑"。
   - **判定手段**：`ffmpeg -i op00.mpg -map 0:v:0 -c copy -f mpeg1video ff.m1v`，
     修正后解出的 ES 与它**逐字节完全一致**（86,606,495 字节）。
   - 本机 `imageio_ffmpeg` 自带的 ffmpeg 可直接当地面真值：真值前 30 帧是纯白渐出（255→214），
     设备解出第 1 帧 Y=235 均匀白、第 11~30 帧 228→198，两者吻合。
4. **MediaCodec 输入契约**：一个输入缓冲必须装**一个完整访问单元**（按 `00 00 01 00` 切帧）；
   按固定字节数切块喂会让 MTK 解出残帧，按帧喂后 `zero=0/480000`。
5. `csd-0`（76 字节序列头）与宽高提示给不给都一样（A/B 实测），不是变量。

**遗留**：M3b 把这条通路接到引擎（解码到 SurfaceTexture/纹理 → 按 `movPlayEx(name,x,y,w,h)`
矩形叠加，并实现 `movWait` / `movPlaying` / `movStop`）；M4 做 MP2 音频（PES 0xC0）、点击跳过与时序。

## D-028 MOV 影片上屏（v0.2.3 / M3b）：后台解码线程 + 引擎合帧时 CPU 合成

**日期**：2026-10-04　**状态**：真机通过

- 新文件 `app/src/main/cpp/android/mov_player.{h,cpp}`：自写 PS 解复用（D-027 的修正版）
  + 按访问单元喂 `video/mpeg2` 解码器（CPU 输出 I420）+ YUV→RGBA（带最近邻缩放）
  → 后台线程帧队列。
- 上屏点选在 **`AndroidGraphicsSystem::EndFrame()`**：把「当前该显示的帧」用
  `AndroidSurface::SetPixelsFromRGBA` + `BlitToSurface` 贴到 `movPlayEx` 给的矩形，
  **完全不动 GL 侧**（Kotlin 渲染器照旧上传帧缓冲，D-022 的内容哈希闸门自然看到变化）。
  代价是每帧一次 CPU 转换 + 一次 memcpy（800x600 毫秒级）；要省 CPU 再上 SurfaceTexture。
- 时序/节流：解码线程最多领先 3 帧（mutex + 条件变量）；引擎线程按墙钟取
  「最后一帧 pts ≤ now」，更早的丢掉（渲染跟不上就跳帧）。影片时钟从「第一帧可用」起算，
  避免解码器预热期白屏。
- 脚本层（`src/modules/module_mov.cc`，上游那个文件此前 7 条全是 `AddUnsupportedOpcode`）：
  实现 `movPlay`(0)/`movPlayEx`(1)/`movWait`(3)/`movPlaying`(4)/`movStop`(5)/`movPlayExC`(20)，
  `movLoop`(2) 仍 unsupported。参数形状由两个游戏的脚本 dump 反推：LBEX 与 KW 都只有
  `op<1:026:00001,0>("OP00",0,0,799,599)` 与 `op<1:026:00020,0>(...)` 两个调用点；
  `movPlay` 没有调用点，暂按同样的 5 参数实现。`movWait` 用 `DefaultIntValue_T<0>` 兼容 0/1 参数。
- 自测开关：`rlvm-diag.txt` 里 `mov_test=OP00`（可加 `mov_test_ms=N`）→ 引擎启动即起播，
  方便在没有脚本触发点的存档/路线里验证（KW 的调用点在 OP 那一段，跑不到就看不到）。
- 真机证据：KW 全屏播 OP，两次截图内容完全不同（蓝天+粉圆 → 山景+日文歌词），
  引擎循环仍 ~123fps（frames=2400 / 19.5s），rss +~10MB。
- 遗留：MP2 音频（M4）；`movPlayExC` 的 C 语义未证实；`movWait` 超时参数未实现；
  CPU 合成若成瓶颈再换 SurfaceTexture。

## D-029 真机日志可见性：应用日志文件 + 面板「日志」视图 + 影片自测开关

**日期**：2026-10-04　**状态**：真机通过

**问题**：真机上看不到 logcat；App 面板里的日志区又小、且**每次重启 App 就清空**，
所以「刚才那轮到底发生了什么」事后无从查证。影片上屏的验证也就没法自证——
只有我这边 adb 能看见 `rlvm-mov` 的日志。

**做法**

1. **日志落盘**：Kotlin 的 `log()` 与 native 的关键事件都追加到
   `<外部文件目录>/rlvm-log.txt`（native 侧走新文件 `android/app_log.{h,cpp}` 的
   `SetAppLogFile` / `AppendAppLogLine`，同时在 `SetDiagnosticsDir` 里设好路径）。
   启动时把文件末尾（64KB）读回面板，重启不再丢。
2. **面板「日志」按钮**：打开整屏日志视图（`复制日志` / `清空` / `关闭`）。
   复制到剪贴板后可以直接粘贴给我，替代 adb/logcat。
3. **面板「影片自测」开关**：翻开关就往 `rlvm-diag.txt` 里写/删 `mov_test=OP00`
   （只动这一行，其它键原样保留），下次点「运行 SAF 引擎」启动就自动播
   `MOV/OP00.mpg`；配合 `mov_test_ms=N` 可限制时长。用途：游戏自己的触发点在脚本
   SEEN514（OP 场景），跑不到那里时也能验证上屏。文档见 `docs/TESTING.md` 的诊断表。

**验证**：真机点「影片自测」→ 写入 `mov_test=OP00`；点「运行 SAF 引擎」→ OP 全屏播放
（截图确认）；应用日志文件里同时出现 Kotlin 行与 native 行：

```
[20:39:18] --- SAF 运行 ---
mov: 起播 MOV/OP00.mpg 矩形 0,0 799x599
mov_test: 起播 MOV/OP00.mpg
```

**注意**：`mov_test` 开着时每次启动引擎都会播影片；看完记得在面板里关掉。
