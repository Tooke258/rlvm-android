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
