# RLVM Android 封装 — 进度交接文档

> ⚠️ **本文写于 2026-09-19（v0.1.0 时代），部分内容已过时。**
> **最新交接请看 [`docs/HANDOFF-2026-10-04.md`](HANDOFF-2026-10-04.md)**
> （含 v0.2.x 之后的全部状态、未提交改动清单，以及 LBEX 存档的待办）。

> 生成时间：2026-09-19 18:00 +08:00
> 用途：上下文被压缩或换人接手时，靠这份文档恢复全部关键结论。
> 配套阅读：`docs/ARCHITECTURE.md`（源码分析）、`docs/ENVIRONMENT.md`（工具链）、
> `docs/DECISIONS.md`（决策 D-001~D-013）、`docs/COMPATIBILITY.md`（游戏与补丁兼容）、
> `docs/TESTING.md`（真机测试流程），以及 `dev-log/*.jsonl`（逐任务日志）。

## 0. 当前焦点（2026-10-05）

**PT00（LBEX 棒球小游戏）走「兼容层」路线：不手写重写，而是在 Android 侧跑一个自研 x86-32
执行器，直接执行原版 `PT00.dll`（DLL 由用户游戏数据在运行时提供，绝不进仓库 / APK）。兼容层已真机跑通。**

当前焦点（2026-10-06）：**小游戏能进、画面能画、原版 DLL 在跑、脚本也在按相位推进，但角色（打者/选手）不渲染**——
场上只有 16 个对象、没有 0×0 源矩形，屏幕上只剩 [SKIP] 热区。以下三条都是被真值修正过的结论，别再重复：

* 引擎**没有**「每帧自动驱动 DLL」的机制；`intG[1900]/[1901]` 是脚本在 `SEEN515` 里、小游戏调用**返回之后**才置位，首次本来就该是 0；
* 相位号正确（真机快照 `intD[73]=10`，相位 10 该写的值都对）→ 卡点不是「分流到等待」；
* 选手贴图都在（`G00/PT_PR_*.g00` 与 `g00_sc/`），`objShow(1004)` 也是实现过的（多行注册，之前 grep 漏了）。

**因此改走「原生 RL 锚点法」**（不比观感，只比脚本可见量）：方案与取数配方在
[`docs/LB-MINIGAME-NATIVE-ANCHORS.md`](LB-MINIGAME-NATIVE-ANCHORS.md)——顺序是 A2（intD 一族）→ A1（对象表）→ A3/A4。

可直接用的取证工具（都不需要真机）：

* `tools/pt00_probe.exe`（**本轮新增**）：**只读**扫原生 `REALLIVE.EXE` 的内存，按签名窗口
  `intD[70..76] = 20,19,15,10,0,-1,1` 反推 `intD` 基址，再把 A2 要的那一族下标整段打出来；
  `--selftest` 已 PASS（找到 + 基址正确），进程名解析与负路径也都测过。构建：`tools\build_pt00_probe.bat`。
  **续（2026-10-06）**：补了 `--scan-all`（不知道进程名也能找）、`--list`、`--find-str <文本>`、
  `--full`（整片 `intD[0..1999]`），运行期消息全 ASCII（cmd 用 CP936 解码 UTF-8 中文会乱码）。
  **已在 PC 实测**：汉化版真引擎进程是 **`lbex_sc.exe`**（不是 `REALLIVE.EXE`），A2 全量已取到（见
  `docs/LB-MINIGAME-NATIVE-ANCHORS.md` §5）。
* `dump_scenes=all`（351 幕反汇编写文件，本地 `build/rlvm-scenes.txt`）、`tools/ida_find_dll_glue.py` +
  `tools/ida_xrefs_of.py`（对 `REALLIVE.EXE` 做调用链取证）、`pt00_trace_ctx`、objbtn 诊断、图形栈转储。
* `tools/g00_decode_probe.exe`（本轮新增，PC 侧）：编译 **APK 同款** vendored 解码器，回答
  「这张图到底解出来是什么」——已用它排除「选手贴图解码成空图」这条。

进度（PC 侧，不需要真机）：

* 三个工具已落地：`tools/pt00_oracle/`（原生真值）、`tools/pt00_emu/`（执行器）、
  `tools/pt00_oracle/compare.py`（逐位对照门禁）；
* 最小夹具 **7/7 逐位一致**；长序列夹具（280 次调用）**280/280 逐位一致**，
  状态级（`state_walk.py` 逐帧比对 `intD+intF` 整片）**280 帧全部一致、0 差异**；
  22 种实体单帧新鲜调用 **22/22 一致**；
* 已修 20+ 个 bug，**全部在宿主执行器**，`PT00.dll` 一行没改。

下一步（需要用户配合）：在 PC 上把原生 RL 走到**小游戏等待画面**，跑
`tools\pt00_probe.exe REALLIVE.EXE`，把输出贴回来 → 与 Android 侧 `[pt00] frame` 快照做 A2 逐项对照。

👉 **细节全部在 [`docs/PT00-EMU-HANDOFF.md`](PT00-EMU-HANDOFF.md)**（契约 / 工具命令 / bug 清单 / 原始差异输出 / 路径速查）。

其余进行中的工作：视频通路（v0.2.3 起）、快进指示符与影片并行的收尾（v0.2.4 起）、
对话栏渲染与影片音画同步（`dev-log/OPEN-ISSUES.jsonl`）。

## 1. 一句话现状

**真实游戏（Kud Wafter）已经在真机上跑出画面：标题背景、START/LOAD/CONFIG/EXIT 菜单、
标题 logo 全部正常渲染，BGM 由游戏自行请求并经 AAudio 播放。**

那条「首屏全黑」的阻塞点已在第 5 节定位并修复：根因是 Android 后端没有实现
`Surface::GetPattern`（GRP type-2 区域表），导致每个图形对象的源矩形都是 0×0。

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
| `HEAD`   | 诊断能力（stdout/stderr → logcat、设备侧诊断文件）+ **修复全黑根因**：GRP type-2 区域表 |

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
| 音频链路 | NWA 解码 → **44.1k→48k 重采样** → 混音 → AAudio；自检 440Hz 源输出 439.5Hz（修复前 478.4Hz） |
| 真实游戏加载 | Kud Wafter：37 KB `GAMEEXE.INI`、3.5 MB `SEEN.TXT`、TOC 62 个场景、`REGNAME=KEY\クドわふたー` |
| **真实游戏渲染** | 标题画面完整呈现（蓝天村庄背景 + START/LOAD/CONFIG/EXIT + 标题 logo），真机截图确认 |
| **游戏自行驱动 BGM** | 日志 `play channel=30 file=BGM/BGM14.nwa`；此后每 300 帧上报一次 `rlvm-audio runtime: active_channels=1 peak_in_window=…`，持续 70 秒以上不停 |
| 诊断可见性 | 上游 `cout`/`cerr` 全部进入 logcat；未实现操作码与被吞掉的异常都可读 |
| **触摸输入（T2.3）** | 真机点标题菜单 START → 进入正文（加载 `ss_mw00*` 正文资源），悬停高亮与光标位置均正确 |
| **文字系统被调用** | 正文第一句触发 `RenderGlyphOnto`（U+5FC3 心 / U+81D3 臓 / U+304C が …），系统 CJK 字体经 FreeType 光栅化 |
| 字形位图正确 | 日志带墨迹统计：`bitmap=26x25 ink=199 max_cov=255`——位图非空、覆盖度满值 |

## 4. 已知缺口

| 缺口 | 说明 |
| --- | --- |
| 文字已光栅化但**未上屏** | 已排除：文本窗口在合成（`Text Area: Rect(47, 460, Size(800, 600))`）、字形位图有墨迹。剩下的是**颜色**：渲染时 colour=(0,0,0)，而 `#COLOR_TABLE.000=255,255,255`（白），`TextWindow` 构造函数本应把它设为默认色——下一步查颜色在哪一步变成黑的 |
| HIK 渲染未接入 | `hik_renderer_` 为空。Kud Wafter 标题流程没用到 HIK；用到 HIK 的作品仍需补 |
| KOE 语音未实现 | 需要先把 KOE/NWK/OVK 语音包的解码链路接上 |
| 通道数不足 | RLVM 只建模 25 个通道，本作用到 channel 30，`SetBgmVolMod` 会抛 `Invalid channel number 30 in channel_volume` 并被跳过 |
| 若干未实现操作码 | Sys 2055 / 2056 / 300 / 1231 / 3503、Os 120；目前一律「跳过继续」，未见功能受损 |
| PNG / JPEG 解码 | 解码器存在但被 `#if HAVE_LIBPNG/JPEG` 排除；RealLive 原生格式用不到 |
| ~~44.1kHz 重采样~~ | **已修复**（D-017）：自建 `ResamplingSource`，44.1kHz→48kHz 线性插值 |
| ~~存档读写经 SAF~~ | **已实现**（见 D-025）：RLVM 存档指令 + `utilities/file.cc` 的 `WriteGameFile`/`ReadGameFileAll`/`GameFileExists` fd 钩子（绝对路径走 posix、相对路径走 SAF）；LBEX 的写入/固化/读取三项已真机验收 |
| **直接杀进程后存档"列不出来"** | 槽位文件 `save%03d.sav.gz` **确实写入成功**，但游戏用于标记"槽位已占用"的 `intG[1050+槽]` 属于 **global memory**，而 global memory 目前**只在引擎正常停止时**才落盘（`RunEngineOn` 末尾；上游对应 `RLVMInstance::Run` 末尾）→ 直接杀进程会丢掉这个标记，LOAD 列表（`SEEN9023` 判定 `intG[1050+n]==1`）便列不出来。修法：把落盘时机扩展到"挂起/退到后台"（复用 `setEngineSuspended`），详见 `dev-log/LBEX-SAVE-LOAD.jsonl` |
| **影片（`MOV/*.mpg`）完全不播** | `src/modules/module_mov.cc` 的 7 条指令（`movPlay` / `movPlayEx` / `movLoop` / `movWait` / `movPlaying` / `movStop` / `movPlayExC`）**全部是 `AddUnsupportedOpcode`**。开场影片在 RLVM 里不播——独立的功能缺口，与帧率/动画平滑度无关。**v0.2.3 主线**：Android 侧 MediaCodec 解码 + 帧回填进引擎 Surface |
| 引擎生命周期 | 只有"跑一段"，没有启动/暂停/恢复/退出 |

## 5. 「首屏全黑」的根因与修复（已解决）

### 现象（修复前）

```
instructions executed = 22395
frames presented = 1686
graphics blits: calls=5732 written_pixels=2400000 nonblack_pixels=0
画面 nonblack = 0/480000            （全黑）
graphics tree dump:
  Object #18:
    Rendering Rect(0, 0, Size(0, 0)) to Rect(0, 0, Size(0, 0))
  Object #21:
    Rendering Rect(0, 0, Size(0, 0)) to Rect(0, 447, Size(0, 0))
```

### 定位过程（按顺序，每一步都用真机证据推翻上一个猜测）

1. **把 stdout/stderr 接到 logcat**（D-014）。这一步立刻显示：脚本在正常推进，
   并且一直在报被吞掉的东西——`Undefined: opcode<1:4:2055, 0>(1)`、
   `Invalid channel number 30 in channel_volume` 等。原先「在等输入 / 在等 HIK」
   的猜测都不成立：追踪显示游戏一路从 SEEN9010（引导）→ 9011（标题渐入）→ 9012
   （标题菜单）走完了。
2. **加图像加载日志**，确认资源链路没问题：`KURO` 800×600、`TT_LOGO_WAR00` 800×600、
   `TT_TTA_BG00` 800×600（576 KB）都成功解码。
3. **加合成计数**（`AndroidSurface::BlitToSurface` 里统计写入像素与非黑像素）：
   5732 次 blit、写入 240 万像素（恰好 5 张全屏）、**非黑像素 0**。
4. **用上游自带的 `GraphicsSystem::Refresh(ostream*)` 转储图形栈**（`dump_graphics=1`），
   决定性证据出现：所有对象的源矩形都是 `Size(0, 0)`。

### 根因

上游对象渲染的源矩形来自 `GraphicsObjectData::SrcRect()`：
`CurrentSurface(go)->GetPattern(go.GetPattNo()).rect`。
Android 后端没有实现 `GetPattern`，基类返回静态空 `GrpRect`，于是每个对象都退化成
0×0 的源矩形——对象、脚本、文件系统全都正常，只是**一个像素都画不出来**。

### 修复

`AndroidSurface` 增加 GRP type-2 区域表（D-015）：加载图像时把 xclannad 解码器的
`region_table`（子图矩形 + 原点偏移）搬进表面，覆写 `GetNumPatterns()` / `GetPattern()`；
没有区域表的资源退化成「整张图一个子图」；区域表保证永不为空（`GetPattern` 返引用）。

### 修复后的真机结果

```
graphics blits: calls=2169 written_pixels=341873182 nonblack_pixels=269250703
frame 800x600 serial=481 nonblack=480000/480000
rlvm-audio: play channel=30 file=BGM/BGM14.nwa loop=1 volume=165   ← 游戏自己请求的标题曲
```

真机截图：标题背景 + START/LOAD/CONFIG/EXIT 菜单 + 「クドわふたー」标题 logo。

### 下一步（按价值排序）

1. **文字上屏**：只差颜色。对照点已知：`#COLOR_TABLE.000=255,255,255`、
   `TextWindow` 构造函数 `SetDefaultTextColor(gexe("COLOR_TABLE", 0))`、
   `ClearWin()` 会把 `font_colour_ = default_colour_`。要查的是渲染时为何是 (0,0,0)：
   记录构造后与每次 `SetDefaultTextColor`/`SetFontColour` 的取值即可。
2. **通道数**：把 `NUM_TOTAL_CHANNELS` 提到本作实际使用范围（30+）或改为按
   `#CHANNEL` 动态分配。注意这属于改动上游语义，需要先记录决策。
3. ~~**44.1 kHz 重采样**~~：已修复（见 D-017），音调不再偏高。
4. **未实现操作码**：Sys 2055 / 2056 / 300 / 1231 / 3503、Os 120。先确认
   `#SEEN_START` 到正文这段是否真的不依赖它们。
5. **引擎生命周期**：目前只有「跑一段」，没有启动/暂停/恢复/退出。

> 已完成：引擎默认不限时运行、可随时停止、重复启动被拒绝；
> 游戏自行请求的 BGM 在整段运行期间持续输出；
> 触摸输入打通（点标题 START 进正文），文字系统首次被调用。

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

先设置 `JAVA_HOME=<JDK 21 路径>` 与 `ANDROID_SDK_ROOT=<Android SDK 路径>`
（已配置为永久用户环境变量），然后在仓库根执行 `.\gradlew.bat assembleDebug --no-daemon`。

**必须提权**：沙箱把 `~/.gradle` 与 `.git` 设为只读，Gradle 构建与 git 提交都要
`require_escalated`。细节见 `docs/ENVIRONMENT.md`。

### 真机

| 项 | 值 |
| --- | --- |
| 设备 | Redmi K40 游戏版（M2012K10C / ares），Android 12 / SDK 31，arm64-v8a |
| adb serial | `<设备序列号>` |
| 真实游戏 | `<游戏目录>`（用户自有，已 SAF 授权；本机真值见 `local-data/LOCAL-PATHS.md`） |
| 夹具位置 | `/sdcard/Android/data/org.rlvm.android/files/probe`（普通路径）与 `/sdcard/Download/rlvm-probe`（SAF） |

### 测试夹具

执行 `powershell -File tools/make_probe_fixture.ps1`，输出到 `build/probe-fixture/`。
内容全部来自 RLVM 上游自带测试数据，**不含任何商业游戏资源**：`Gameexe.ini`
（追加 `#DISKMARK` 与 `#FOLDNAME`）、`Seen.txt`、`SEEN0002.TXT`（按 TOC 切出的
真实场景字节）、`test.wav`（脚本生成的 440Hz 正弦）、`g00/test.g00`（脚本生成的
320x240 BMP，格式由内容判定）、`g00/doesntmatter.g00`（上游 `test/Gameroot`）。

### 界面与日志

应用有四个按钮：选择游戏目录 / 运行 SAF 引擎 / 运行应用目录（诊断）/ 停止引擎。
**引擎默认不限时运行**（停在标题或正文上，BGM 才会持续），由「停止引擎」按钮收尾；
同时只允许一台引擎运行，重复点击会被拒绝。自动化测试要在报告里拿到结果时，
用诊断文件设 `time_budget_ms`。
日志标签：`rlvm-native`、`rlvm-audio`、`rlvm-gl`、`rlvm-font`、`rlvm-graphics`，
以及**上游诊断输出** `rlvm-stdout` / `rlvm-stderr`（见下方诊断文件）。
可用 `uiautomator dump` 取控件 bounds 后 `input tap` 自动点击，无需人工。
截图：`adb shell screencap -p /sdcard/s.png` + `adb pull`——**不要**用
PowerShell 的 `>` 重定向，它会把二进制流改坏（见第 7 节）。

### 设备侧诊断文件（不用重新构建）

把同名文件 push 到 `/sdcard/Android/data/org.rlvm.android/files/rlvm-diag.txt`，
点「运行 SAF 引擎」即可生效；文件不存在时全部取缺省值。模板见
`tools/rlvm-diag.sample.txt`：

```
trace=1            # 逐条指令追踪（上游 set_tracing_on），走 rlvm-stderr
dump_graphics=1    # 运行结束时转储图形栈（src/dst 矩形、alpha、可见性）
time_budget_ms=25000
max_instructions=200000
frame_log_every=120
```

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
| 部分移动 GPU 要求 `#version` 在第一行 | 规范允许前置空白，这类驱动不接受；且没查编译状态，失败是静默的 | 着色器用 `trimIndent()`；必须检查编译/链接状态。（**测试机实际是联发科天玑 1200 / Mali GPU**，早前文档里写的 Adreno 是误记） |
| `fs::file_size` 对目录 | Linux 允许，Android 抛 `Function not implemented` | 只对普通文件取大小 |
| `CorrectPathCase` 会遍历真实目录 | 真实 Gameexe 有 CG 表条目，SAF 下路径不存在直接抛异常，整台引擎装配失败 | 资源读取一律经 `GameFileSystem` |
| 上游 `MakeConverter` 把**源速率**也传成 48k | `SDL_BuildAudioCVT(cvt, fmt, ch, freq, fmt, 2, freq)`：源速率写成了目标速率 48k，SDL 判定「无需转换」；自带的 `conv_wave_rate` 又只在「目标 < 源」时生效。于是 44.1kHz 音源按 48kHz 播，快 8.8%（音调高约 1.5 个半音） | 不改上游：在音源外面套 `ResamplingSource`（D-017）；用 `audio_selftest=1` 把「输出频率」变成日志里的数字 |
| 极简夹具掩盖问题 | 空 `REGNAME`、无 CG 表、无 `#DISKMARK`——四个 bug 直到接真实游戏才暴露 | 关键路径要用真实数据验证，但商业资源不进仓库 |
| PowerShell 5.1 按 ANSI 读无 BOM 的 `.ps1` | 中文注释被误解码后吞掉后续行 | 仓库内 `.ps1` 一律纯 ASCII |
| FreeType 模块表须与编译文件一致 | `ftmodule.h` 列了 19 个模块，少编一个就链接失败 | 用自定义 `ftmodule_android.h`；可变字体支持还需编 `ftmm.c` |
| **surface 未实现 `GetPattern` 会导致整屏全黑** | 对象、脚本、图像加载全部正常，blit 上万次，写入的像素却 100% 是黑的 | 源矩形来自 `GetPattern`，基类默认返回 0×0 矩形；必须实现 GRP type-2 区域表（D-015） |
| PowerShell 的 `>` 重定向会损坏 `adb exec-out screencap` 的 PNG | 图片打不开（`invalid or unsupported image data`） | 用 `adb shell screencap -p /sdcard/x.png` + `adb pull` |
| 只看「最终帧非黑像素数」无法定位渲染问题 | 分不清「没画」「画了但透明」「画了确实是黑的」 | 加合成计数（写入像素/非黑像素）+ 上游 `Refresh(ostream*)` 图形栈转储 |
| **脚手架验证代码会破坏真实行为** | 之前为验证音频链路主动 `BgmPlay("BGM01")` + `BgmStop()`：真机表现是「开头能听到一点，随后被测试音乐打断，然后彻底没声音」 | 游戏能自行驱动后立刻删掉这段；证据改为运行期周期上报，不做任何主动干预 |
| 不限时运行需要「喊停 + 防重入」 | 默认持续运行后，重复点「运行」会起第二台引擎，两台共用 AudioEngine/帧缓冲互相踩踏 | 加 `requestStop()` 标志 + `RunGuard` 唯一运行权，重复点击返回明确错误 |

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
2. 安装：`adb -s <设备序列号> install -r app\build\outputs\apk\debug\app-debug.apk`。
3. 清日志并启动：`adb -s ... logcat -c`、`am force-stop org.rlvm.android`、
   `am start -n org.rlvm.android/.MainActivity`。
4. 自动点击：`uiautomator dump` 后 `pull` 出 UI XML，取 `运行 SAF 引擎` 的 bounds
   中点，`input tap <x> <y>`。
5. 读结果：`adb -s ... logcat -d -s rlvm-native:V rlvm-audio:V rlvm-graphics:V rlvm-stderr:V '*:S'`。
6. 需要更细的观察时，改 `rlvm-diag.txt` 再 push（见第 6 节），无需重新构建：
   打开 `trace` 看指令流，打开 `dump_graphics` 看每个对象的 src/dst 矩形。
7. 肉眼确认：`adb shell screencap -p /sdcard/s.png` + `adb pull`，**不要**用 shell 重定向。

SAF 目录授权需要人工在系统选择器里点一次（SAF 的固有环节，无法绕过）；
授权会持久化，之后无需重复操作。

## 10. 2026-10-06 里程碑：LBEX 小游戏全链路打通

**最新交接请看 [`docs/HANDOFF-2026-10-06.md`](HANDOFF-2026-10-06.md)**（本日归档）；
每一处 bug 的现场/日志/反汇编证据在 [`docs/PT00-EMU-HANDOFF.md`](PT00-EMU-HANDOFF.md) §10。

**一句话**：小游戏不再卡 `time`，人物与球正常渲染，第一场（脚本驱动）跑完 → 结算 →
后续剧情文本正常出现并能继续点击推进。用户真机确认「现在游戏能正常推进了」。

修掉的真 bug（全部有真机 / 夹具证据）：

| # | 一句话根因 |
| --- | --- |
| 1 | 入口 `reallive_dll_func_call` 是 stdcall（`retl $0x14` 已清参数），执行器又清一次 → `esp` 每次 CallDLL 漂 +20 字节（8963 次后 `0x10200000 → 0x1022bc40`） |
| 2 | `cmp r32, r/m32`（opcode `0x3B`）寄存器形式被截断低 16 位 → vector 前缀拷贝 `p != pos` 永远成立 → 死循环（观察点抓到入口现场才定位） |
| 3 | x87 `DA` 组（m32int 整数算术）整族未实现 → 每次 `UNIMPL` 让整个 CallDLL 中途放弃 → "能画但逻辑不推进" |
| 4 | 汉化版 GAMEEXE.INI 缺窗口属性（020/021/031/032）→ `TextWindow` 构造抛异常 → `Msg 17 = pause` 永不完成 → 画面冻结、音乐正常、55Hz 空转 |
| 5 | 上游 `TextWindow::Render` 先解引用后判空 → 名牌 waku 空指针崩溃（靠 tombstone 符号定位） |

新增取证设施：观察点 `pt00_watch=<hex>`（寄存器 + `[edi-0x10..+0x1c]` 环形转储）、
`INTD 读环`、`mem edi/esp/esi` 窗口、`emu.exe --selftest`（cmp / x87 指令级回归）、
崩溃处理器改为**链回系统处理器**（保留 tombstone）+ `_Unwind_Backtrace` 模块内偏移回溯。

仍待处理见 `HANDOFF-2026-10-06.md` §5（结算段未实现 opcode、`lb_child_obj_1058` 四条 op、
表现层对齐、小游戏 ~16fps 等）。

---

## 2026-10-06 夜 · 本轮交付（归档）

**已闭环并真机验证**

1. **左视窗地图消失**（`bgrLoadHaikei("?",230)` 的"对象晋升"把地图擦掉）：
   根因是 `GraphicsObject::Impl` **拷贝构造函数**里 `wipe_copy_(0)` —— 任何参数 setter 的
   首次写入（`objMove/objShow/…`）都会清掉 `objFgWipeCopyOn` 打的保护。
   修：`graphics_object.cc` 改成 `wipe_copy_(rhs.wipe_copy_)`（+9/-1，仅此一处）。
   真机 A/B：晋升汇总 `fg_freed` 109 → **60**；`#201` 一路 `vdw1`。见
   `dev-log/MINIGAME-MAP-LOSS.jsonl`、`docs/MINIGAME-STATIC-RECON.md` §9。
2. **方向键**：pad 事件本来就到引擎，缺的是"引擎写 intD"。实测标定
   **`intD[104]=上 / 105=右 / 106=下 / 107=左`**（103 = 另一动作位）；
   实现为"按住每帧写 1、松开写 0"。见 `docs/INPUT-KEY-RECON.md` §8.1。
3. **挥棒**：`intD[101]` 直连已通（84 次电平切换）。
4. **pad 两个缺陷**：推光标带 `buttons=1`（被当成左键）、方向键手势 MOVE 泄漏成游戏点击
   （"按方向键像按左键"的真凶）。均已修。

**新证据 / 新工具（都在库里）**

* `[wipe]` 逐对象晋升/擦除日志 + 195..255 号对象活体时间线（`wipe_log=1`）。
* **白名单逐指令 trace**（`op_trace=a,b,c`，默认不过滤时行为不变）。
* `input_trace=1`：每帧变化时打 `keys=/cursor=/intD95_115=`，用来区分"按键没到引擎"与
  "引擎没写 intD"。
* `intd_dir_poke` / `intd_hit_poke` / `intd_poke`（+`intd_poke_free`）—— 直接写 intD 的标定器。
* PC 侧：`tools/input_watch.py`（连续采样 + 噪声基线 + ★新变化★）；`pt00_probe.exe` **锚定模式**
  （`<proc> <intD[73]的值> 6 --full`）在签名失效时也能锁定（本轮 intD 基址 `0x97a524`）。

**进行中 / 下一步（优先级）**

1. **「看不到球、看不到猫」**：根因已定位到**练习选择菜单不渲染**（`PT_PR_*` 那套世界内对象）。
   进打击练习（模式 `30/31/32`）才能出手（`intD[76]==2 && intD[630]==58` → `intD[40]=123`）。
   现场取证：PC 上调出该菜单 → 手机同画面导渲染树 → 对 `SEEN7111` 逐 op 核对。
2. 把方向键/挥棒**固化成正式实现**（默认开）+ 回归（四方向/挥棒/菜单导航/Ctrl 快进）。
   ——✅ 已于 2026-10-07 固化（`docs/INPUT-KEY-RECON.md` §8.6），真机回归待跑。
3. **卡顿**：已量清是"读图"（`loaded` 前平均 96.5ms、最大 415ms），
   修法 = SAF 目录清单+句柄缓存 + 预取。见 `dev-log/PERF-STUTTER.jsonl`。
4. 仍挂着的：`Sys 460/461` 等空桩真实语义、`GanFg 101/102`、`ChildObjFg 1058`、
   小游戏实体渲染的整体核对（`PT_PR_*`/`PT_Y*`）。

**诊断配置现状（重要）**：设备上的 `rlvm-diag.txt` 当前是**取证配置**
（`lb_minigame=1 / input_trace=1 / intd_dir_poke=1 / intd_hit_poke=1`，
样例 `tools/rlvm-diag.input-poke.txt`）。准备推包/发版前要换回常规配置或删掉该文件。

---

## 2026-10-07 凌晨 · 输入固化（本轮交付）

**做了什么**：把上一轮用诊断开关标定出来的「方向键 / 挥棒 → intD」转正为默认实现，
不再依赖设备上的 `rlvm-diag.txt`。

* `intd_dir_poke` / `intd_hit_poke` 默认 **true**；方向槽位改成实机标定值
  `up=104 / down=106 / left=107 / right=105`（代码里的旧默认 103..106 是错的，
  只有 diag 文件覆盖过才对）。
* 依据：`build/rlvm-scenes.txt` 全库扫描确认只有 `SEEN7420` 读 `intD[101]` 与
  `intD[103..107]`，所以常态写入无副作用；写入语义仍为"按住每帧写 1、松开写一次 0"。
* `[input] dir_poke:` / `[input] hit_poke:` 日志改为只在 `input_trace=1` 时输出，
  避免常态点击刷日志。
* 验证：`.\gradlew.bat assembleDebug` 通过（BUILD SUCCESSFUL in 20s，仅既有 warning）；
  **真机未跑**。

**下一步（顺序未变）**

1. 练习选择菜单不渲染（`PT_PR_*`）——需要 PC 侧现场 + 手机同画面渲染树（`SEEN7111` 逐 op）。
2. 真机回归：四方向 / 挥棒 / 菜单导航 / Ctrl 快进。
3. 卡顿：SAF 目录清单 + 句柄缓存 + 预取（`dev-log/PERF-STUTTER.jsonl`）。
4. 长尾：`intD[103]` 跑步映射、`Sys 460/461`、`GanFg 101/102`、`ChildObjFg 1058`。

---

## 2026-10-07 凌晨（通宵）· 完美收集档 + 相册 Scene 回想修复（本轮交付）

提交：`227a22f`（本地，未推送）。细节见 `docs/SAVE-STRUCTURE-ANALYSIS.md` §11 与
`dev-log/SCENE-REPLAY-HANG.jsonl`。

### A. 相册三栏"全解锁"找到了真源（并做成可复用的 diag）

| 栏 | 真源 | 判定 |
| --- | --- | --- |
| Gallery(CG) | `CGMTable::cgm_data_`（`dat/mode.cgm` 的 文件名→flag） | `SEEN9515` 用 `Sys 1504`(cgStatus) 逐条问 |
| Scene | intZ **位** `390*32 + 0..15` | `SEEN9517` 里 14 条硬编码条目逐条判 `==1` |
| Music | intZ **位** `391*32 + 0..55` | `SEEN9516` 里 56 条逐条读 |

* 新增 diag（`rlvm-diag.txt`，默认关）：`cgm_unlock_all` / `collection_unlock_all` /
  `kidoku_unlock_all` / `cgm_dump`（+`CGMTable::MarkAllViewed()`）。
  一次启动的顺序固定为 **kidoku → cgm → collection**。
* ★顺序坑：`CGMTable::SetViewed` 是按"**字**"写 `intZ[flag]=1`（flag=0..409），
  而 Scene/Music 用的是"**位**"。先跑 collection 再跑 cgm 会把音乐位抹掉（当时表现为"音乐只有一半"）。
* 手改 boost 文本归档不可靠（前几轮就是这么干的）；正确姿势是让**引擎自己写**再 `saveGlobalMemory`。
* 产物：`build/global-perfect-engine.sav.gz`（37224 token：cgm_data_ 410 + Scene 位全亮 +
  Music 位全亮 + 满 kidoku），设备上留了 `.perfect / .preflagtest / .kidokutest / .cgmtest / .beforeperfect` 多份备份。

### B. ✅ 相册 Scene 回想"点进去必卡死"结案 —— 根因在我们自己的音频层

```
SEEN9517 入口第 284 行 = op<1:020:00106,1> = bgmFadeOutEx()
  → RLVM 的 "Ex" 版推 WaitLongOperation，判据 sound().BgmStatus() == 0（等 BGM 真停）
  → 我们的 AndroidSoundSystem::BgmFadeOut 只做【音量】淡到 0，通道一直算 playing
  → BgmStatus() 恒为 1 ⇒ 长操作永不返回 ⇒ 引擎不再派发指令（不是死循环）
```

放大器：音频回调里"淡出完成→playing=false"写在读环形缓冲**之后**，`got==0` 直接
`continue` ⇒ 缓冲一空就永远不会处理停通道请求。

修复（只动 Android 音频层）：新增 `AudioEngine::FadeOutAndStop(ch, ms)`（音量线性淡出 +
到点**真正停通道**，对齐 SDL `Mix_FadeOutMusic`），`BgmFadeOut` 改调它；回调把"到点停"
挪到读缓冲之前，并在 `got==0` 且已请求停播时立即停。真机验证：修前
`longop push: 17WaitLongOperation @(SEEN9517)(Line 284)` 之后再无日志；修后紧跟
`longop pop`，随后 `loop_probe` 转到 `SEEN9030/SEEN2801`，屏幕出现回想正文。

### C. 本轮新增的可复用探针（重要，别再走弯路）

* `loop_detect=1`：每 2 秒往**应用日志**写一行"当前最热的两条 (场景,行号)"。
  **不再新增行 = 引擎停摆（阻塞），不是空转**。
* `longop_log=1`：长操作 push/pop 各一行并带 C++ 类型名。**最后只有 push 没有 pop =
  卡在"等输入/等某动作"**，类型名直接告诉是哪一种。本次就是靠它一行定位。
* 教训：逐指令 trace(`op_trace=*`) 与引擎报错走的是 **logcat**（tag `rlvm-stderr`/
  `rlvm-stdout`），**不进** `rlvm-log.txt`，且 logcat 是环形缓冲（约 6 万行）——
  之前"日志里 grep 不到 Undefined"就是这个误判；另外 `op_trace` 有打印预算，
  预算被相册刷帧循环吃光后尾部会截断，别把"最后一行"当结论。
* entrypoint 两套编号的坑：dump 的 `#entrypoint N` 是 **kidoku 表下标**，
  `farcall(scene, X)` 的 X 是真实 id = `kidoku_table[N]-1000000`
  （核对工具 `tools/seen_entrypoints.py`）。

### D. 仍挂着（下次可挑）

1. `Sys 457 / 2402 / 2502 / 1520 / 1521 / 366 / 801` 仍未实现（本次卡死与它们无关）。
   语义要反 `REALLIVE.EXE`（IDA 根目录 `E:\BaiduNetdiskDownload\IDA\IDA_Pro_v8.3_Portable`，
   游戏目录里已有 `REALLIVE.EXE.i64`）。已探明：裸字节 xref 追不动
   （`CGTABLE_FILENAME` 在 VA 0x631c3d 但 .text 里零引用），要上 IDA；
   另注意 **PC 汉化版读的是 `seen_sc.bin`，与我们的 `SEEN.TXT` 不是同一份脚本**。
2. 小游戏那条线（练习选择菜单不渲染 `PT_PR_*`、实体渲染、按键固化后真机回归）。
3. 卡顿：SAF 目录清单 + 句柄缓存 + 预取（`dev-log/PERF-STUTTER.jsonl`）。
4. 收尾推包：完美档 + 回想修复 + 新 diag 要不要合成一版推上去（本次只做了本地提交）。
5. 设备侧 `rlvm-diag.txt` 现在是 **loop_detect=1 + longop_log=1** 的取证配置，
   推包/发版前要换回常规或删掉。
