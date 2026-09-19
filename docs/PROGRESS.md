# RLVM Android 封装 — 进度交接文档

> 生成时间：2026-09-19 18:00 +08:00
> 用途：上下文被压缩或换人接手时，靠这份文档恢复全部关键结论。
> 配套阅读：`docs/ARCHITECTURE.md`（源码分析）、`docs/ENVIRONMENT.md`（工具链）、
> `docs/DECISIONS.md`（决策 D-001~D-013）、`docs/COMPATIBILITY.md`（游戏与补丁兼容）、
> `docs/TESTING.md`（真机测试流程），以及 `dev-log/*.jsonl`（逐任务日志）。

## 1. 一句话现状

**基础设施与全部平台后端已经打通并在真机上验证：游戏能加载、脚本能执行、画面能呈现、
声音能出声。但真实游戏的首屏仍然全黑——脚本在观察窗口内什么都没画。**

这不是"没接通"，而是"游戏还没走到绘制那一步"。阻塞点见第 5 节。

## 2. 里程碑与提交（旧 → 新）

| 提交 | 内容 |
| --- | --- |
| `b87ff44` | 基线：RLVM 上游源码 + T0.1 架构分析 + 工具链审计 |
| `03e86f7` | 永久环境变量、Gradle/NDK/git 三项验证、决策记录 |
| `b8a50c0` | T0.2/T0.3：Android 骨架可离线构建、AGP 9 原生配置、项目文档 |
| `a40462c` | debug 变体加入 x86_64 ABI（为模拟器预留；release 仍只有两个 ARM ABI） |
| `1f7c1c4` | T1.1/T1.3：上游 149 个源文件 + Boost 编入 `librlvm.so`，两个 ABI 均通过 |
| `cf9be2b` | T1.4：首次在真机上执行 RLVM 解析核心 |
| `db5be02` | T2.4：`AndroidSystem` 顶替 `SDLSystem`，引擎执行字节码 |
| `51b96db` | T2.2/T3.1：SAF 文件访问 + 目录授权持久化 |
| `e88333b` | `SEEN####.TXT` 场景覆盖（汉化补丁机制）经 SAF 生效 |
| `2704efd` | 大小写不敏感文件名解析（两个后端） |
| `1169b7d` | 游戏资源查找层抽象（`GameFileSystem`） |
| `e7e0b78` | T3.3：GLES 呈现链路打通 |
| `da3c500` | T4.2a：图像解码（PDT/G00/BMP）并显示 |
| `5aa5dea` | T4.1：AAudio 音频后端（流式解码 + 无锁环形缓冲） |
| `4a53ca0` | 真实游戏联调：修复四个被极简夹具掩盖的 bug |
| `2657960` | T4.1b：`AndroidSoundSystem` 接到 `AudioEngine` |
| `c4e9ef9` | 文字渲染（FreeType）+ 报告日志修复 |

## 3. 已验证的能力（每条都有真机证据）

| 能力 | 证据 |
| --- | --- |
| 交叉编译与打包 | 两个 ABI 的 `librlvm.so` 均构建通过，APK 含 arm64-v8a / armeabi-v7a |
| 引擎解析核心 | 真机解析 `Gameexe.ini`、打开 SEEN 归档、构造场景（含解压） |
| 引擎执行字节码 | `AndroidSystem` + `RLMachine` + 全部指令模块，真机执行上千条指令 |
| SAF 文件访问 | 目录列举、`Gameexe.ini` 整体读入、`SEEN.TXT` 经 fd + mmap；无任何存储权限 |
| SAF 授权持久化 | `takePersistableUriPermission` + SharedPreferences，重启后仍可访问 |
| 场景覆盖（补丁机制） | `SEEN0002.TXT` 使 TOC 从 `[1]` 变为 `[1,2]`，两条后端一致 |
| 大小写不敏感解析 | 全小写夹具（`seen.txt`/`gameexe.ini`）仍能按标准名找到 |
| 资源查找层 | `FindFile(doesntmatter, g00)` → `g00/doesntmatter.g00`，可用 fd 打开 |
| 帧呈现 | CPU 合成 → 帧缓冲 → JNI → GL 纹理 → 屏幕，截图确认 |
| 图像解码 | 320x240 BMP 经 `GRPCONV` 解码并正确合成（通道序已修正） |
| 音频链路 | NWA 解码 → 混音 → AAudio → 蓝牙输出，峰值与源一致（用户已确认听到） |
| 真实游戏加载 | Kud Wafter：37 KB `GAMEEXE.INI`、3.5 MB `SEEN.TXT`、TOC 62 个场景、`REGNAME=KEY\クドわふたー` |

## 4. 已知缺口

| 缺口 | 说明 |
| --- | --- |
| 文字从未被渲染 | `FontEngine` 已实现并链接，但引擎在观察窗口内一次都没调用文字系统 |
| 音频从未被游戏触发 | `AndroidSoundSystem` 已接通，但引擎没请求过 BGM/SE；听到的声音来自显式验证触发 |
| HIK 渲染未接入 | `hik_renderer_` 为空，`BACKGROUND_HIK` 分支会退回 DC0（黑） |
| KOE 语音未实现 | 需要先把 KOE/NWK/OVK 语音包的解码链路接上 |
| GRP type-2 多子图 | 区域表未接入，`Surface::GetPattern` 仍是基类默认值 |
| PNG / JPEG 解码 | 解码器存在但被 `#if HAVE_LIBPNG/JPEG` 排除；RealLive 原生格式用不到 |
| 44.1kHz 重采样 | 上游不对低于 48kHz 的音源重采样，真实 BGM 会播快约 8.8%（已确认） |
| 触摸输入 | `EventSystem` 是桩，从不注入点击（很可能是首屏无进展的原因之一） |
| 存档读写经 SAF | 未实现 |
| 引擎生命周期 | 只有"跑一段"，没有启动/暂停/恢复/退出 |

## 5. 当前阻塞点与下一步（最重要）

### 观察到的现象

真实 Kud Wafter（场景正确地从 `#SEEN_START=9010` 启动）：

```
engine assembled (regname="KEY\クドわふたー")
instructions executed = 1668
frames presented = 174          （约 58fps，稳定）
stop reason = time budget exhausted
halted = no
画面 nonblack = 0/480000        （全黑）
文字系统调用 = 0
引擎请求音频 = 0
```

### 三种可能，按可能性排序

1. **在等待输入**：`EventSystem` 从不注入点击，标题画面可能在死等一次点击。
2. **用 HIK 渲染画面**：`hik_renderer_` 未接入，`DrawFrame` 在 `BACKGROUND_HIK`
   分支下拿不到渲染器就退回 DC0（全黑）。
3. **每条指令都在抛被吞掉的异常**：`halt_on_exception(false)` 会静默跳过，
   而 `std::cout`/`std::cerr` 在 Android 应用里默认被丢弃，所以看不到。

### 下一步该做什么

**把引擎的 `std::cout` / `std::cerr` 重定向到 logcat**（native 侧替换 streambuf，
转发给 `__android_log_print`）。Android 应用的原生标准输出默认丢弃，这是目前所有
"看不见"的诊断被卡住的根因。做完之后：

- `machine.SetPrintUndefinedOpcodes(true)` 的输出就能看到；
- 可以直接判断是上面三种情况中的哪一种；
- 也能顺手用 `machine.set_tracing_on()` 做逐条指令追踪。

若确认是 (1)，T2.3 触摸输入就从"可选"变成"必需"；
若是 (2)，则要在 `AndroidGraphicsSystem` 里接入 HIK 渲染器。

## 6. 工程事实速查

### 目录

```
rlvm-release-0.14/                工作区根（同时也是 git 仓库根）
├── app/src/main/cpp/
│   ├── native-bridge.cpp         JNI 显式注册 + 探针入口
│   ├── android/                  Android 平台后端（16 个文件，见第 8 节）
│   └── compat/                   SDL 音频 shim、FreeType 精简模块表
├── rlvm-release-0.14/            RLVM 上游源码（只读基线）
├── third_party/                  gitignored：freetype / ogg / vorbis
├── build/probe-fixture/          测试夹具（脚本生成）
└── docs/  dev-log/  tools/
```

### 构建

先设置 `JAVA_HOME=E:\STM32cubeMX\jre` 与 `ANDROID_SDK_ROOT=E:\DEV\AndroidSdk`
（已配置为永久用户环境变量），然后在仓库根执行 `.\gradlew.bat assembleDebug --no-daemon`。

**必须提权**：沙箱把 `~/.gradle` 与 `.git` 设为只读，Gradle 构建与 git 提交都要
`require_escalated`。细节见 `docs/ENVIRONMENT.md`。

### 真机

| 项 | 值 |
| --- | --- |
| 设备 | Redmi K40 游戏版（M2012K10C / ares），Android 12 / SDK 31，arm64-v8a |
| adb serial | `if6lf67xkfs4ibf6` |
| 真实游戏 | `/sdcard/Download/GAL/库特Wafter/库特Wafter`（用户自有，已 SAF 授权） |
| 夹具位置 | `/sdcard/Android/data/org.rlvm.android/files/probe`（普通路径）与 `/sdcard/Download/rlvm-probe`（SAF） |

### 测试夹具

执行 `powershell -File tools/make_probe_fixture.ps1`，输出到 `build/probe-fixture/`。
内容全部来自 RLVM 上游自带测试数据，**不含任何商业游戏资源**：`Gameexe.ini`
（追加 `#DISKMARK` 与 `#FOLDNAME`）、`Seen.txt`、`SEEN0002.TXT`（按 TOC 切出的
真实场景字节）、`test.wav`（脚本生成的 440Hz 正弦）、`g00/test.g00`（脚本生成的
320x240 BMP，格式由内容判定）、`g00/doesntmatter.g00`（上游 `test/Gameroot`）。

### 界面与日志

应用有三个按钮：选择游戏目录 / 运行 SAF 引擎 / 运行应用目录（诊断）。
日志标签：`rlvm-native`、`rlvm-audio`、`rlvm-gl`、`rlvm-font`。
可用 `uiautomator dump` 取控件 bounds 后 `input tap` 自动点击，无需人工。

### 上游改动

`git diff b87ff44 HEAD -- rlvm-release-0.14`：**15 个文件、195 行新增、43 行删除**，
全部是新增重载或最小修复，没有改动 `libreallive` / `machine` / `modules` 的语义。

## 7. 踩过的坑（避免重复）

| 坑 | 表现 | 结论 |
| --- | --- | --- |
| logcat 单条消息约 1000 字符上限 | 整份报告塞进一次 `__android_log_print`，尾部被静默丢弃，把"3 秒正常跑完"误判成"卡死" | 报告必须逐行输出（`LogReport`） |
| JNI 的 `NewStringUTF` 要求 Modified UTF-8 | 游戏 `#REGNAME` 是 Shift-JIS，直接传会让 ART abort 进程（表现为"跑一下就被杀"） | 所有 native→Java 字符串走 `NewSafeJavaString` |
| JNI 回调未清理 Java 异常 | Kotlin 抛异常后 `CallIntMethod` 返回未定义值，对不存在的文件返回了"有效" fd | 每次回调后 `ClearJavaException`；Kotlin 侧也要 try/catch |
| SAF 每文件一次跨进程查询 | 索引 1600 个文件耗时 61 秒 | 目录列举自带类型标志，降到 2.2 秒 |
| C++ 成员初始化顺序 | `gameexe_` 声明在子系统之后，子系统构造时取到未初始化引用 → SIGSEGV | 被依赖的成员必须声明在最前 |
| Adreno 要求 `#version` 在第一行 | 规范允许前置空白，高通驱动不接受；且没查编译状态，失败是静默的 | 着色器用 `trimIndent()`；必须检查编译/链接状态 |
| `fs::file_size` 对目录 | Linux 允许，Android 抛 `Function not implemented` | 只对普通文件取大小 |
| `CorrectPathCase` 会遍历真实目录 | 真实 Gameexe 有 CG 表条目，SAF 下路径不存在直接抛异常，整台引擎装配失败 | 资源读取一律经 `GameFileSystem` |
| 上游不对 <48kHz 音源重采样 | 真实 BGM 是 44.1kHz，会播快约 8.8% | 待修：给 `WavFileSource` 套重采样装饰器 |
| 极简夹具掩盖问题 | 空 `REGNAME`、无 CG 表、无 `#DISKMARK`——四个 bug 直到接真实游戏才暴露 | 关键路径要用真实数据验证，但商业资源不进仓库 |
| PowerShell 5.1 按 ANSI 读无 BOM 的 `.ps1` | 中文注释被误解码后吞掉后续行 | 仓库内 `.ps1` 一律纯 ASCII |
| FreeType 模块表须与编译文件一致 | `ftmodule.h` 列了 19 个模块，少编一个就链接失败 | 用自定义 `ftmodule_android.h`；可变字体支持还需编 `ftmm.c` |

## 8. Android 后端代码结构

| 文件 | 职责 |
| --- | --- |
| `native-bridge.cpp` | JNI 显式注册；探针入口（`probeGameDir` / `runScenario` / `runScenarioSaf`）；帧呈现缓冲；`LogReport`、`NewSafeJavaString` |
| `android/android_system.*` | `AndroidSystem` + `EventSystem`（真实时钟/休眠）+ `TextSystem` + `TextWindow` + `SoundSystem`（接 `AudioEngine`） |
| `android/android_graphics.*` | `AndroidSurface`（CPU 像素表面，含 `BlendCoverage`）、`ColourFilter`、`GraphicsSystem`（帧缓冲 + 图像解码） |
| `android/audio_engine.*` | `FrameRing`（无锁 SPSC）、`AudioSource`、`WavFileSource`、`AudioEngine`（AAudio + 解码线程 + 混音 + 音量渐变） |
| `android/font_engine.*` | FreeType 封装：系统 CJK 字体加载、字形缓存、推进量、光栅化 |
| `android/saf_file_system.*` | SAF 门面（`SafBackend` 接口 + `SafReadAll` / `ReadGameFile`） |
| `android/jni_saf_backend.*` | 把 Kotlin 的 `SafFileSystem` 适配成 `SafBackend`；缓存 jmethodID；清理 Java 异常 |
| `android/game_file_system.*` | 资源查找抽象：普通路径后端与 SAF 后端 |
| `compat/sdl_shim/` | 最小 `<SDL/SDL_mixer.h>`，供 vendored `wavfile.cc` 做 PCM 转换 |
| `compat/freetype/` | 精简 FreeType 模块表 |

## 9. 复现一次完整验证

1. 构建：在仓库根执行 `.\gradlew.bat assembleDebug --no-daemon`（需提权）。
2. 安装：`adb -s if6lf67xkfs4ibf6 install -r app\build\outputs\apk\debug\app-debug.apk`。
3. 清日志并启动：`adb -s ... logcat -c`、`am force-stop org.rlvm.android`、
   `am start -n org.rlvm.android/.MainActivity`。
4. 自动点击：`uiautomator dump` 后 `pull` 出 UI XML，取 `运行 SAF 引擎` 的 bounds
   中点，`input tap <x> <y>`。
5. 读结果：`adb -s ... logcat -d -s rlvm-native:V rlvm-audio:V rlvm-font:V '*:S'`。

SAF 目录授权需要人工在系统选择器里点一次（SAF 的固有环节，无法绕过）；
授权会持久化，之后无需重复操作。
