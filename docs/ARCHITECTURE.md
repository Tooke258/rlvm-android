# RLVM Android 封装 — 源码架构分析（T0.1）

> 状态：只读分析完成，未修改上游任何源码。
> 分析对象：`rlvm-release-0.14/rlvm-release-0.14/`（RLVM 上游，GPLv3）
> 分析日期：2026-09-19
> 对应任务：T0.1（只读分析 RLVM 源码，识别核心模块、桌面依赖、需重写部分）
> 相关文档：`docs/ENVIRONMENT.md`（本机工具链现状探测记录）

## 1. 结论摘要

1. RLVM 的核心（`libreallive` 解析 + `machine` 虚拟机 + `modules` 指令实现，约 3.5 万行）与平台无关，Android 侧不应改动其语义。
2. 平台相关代码收敛在一个清晰抽象层：`src/systems/base/`（接口）+ `src/systems/sdl/`（SDL 1.2 实现）。Android 移植 = **新增 `systems/android/` 后端** + 丢弃 3 个桌面壳层。
3. 唯一的硬编码后端实例化点是 `src/machine/rlvm_instance.cc` 中的 `SDLSystem sdlSystem(gameexe);`，这是必须修改的**最小侵入点**。
4. 三个真正昂贵的改造：
   - **渲染**：SDL 后端使用 OpenGL 1.x 立即模式 + ARB 扩展（`glBegin`/`glVertex2i`/`glActiveTextureARB`/`glCreateProgramObjectARB`），OpenGL ES 3.0 下完全不可用，纹理/绘制/着色器路径需重写。
   - **文件系统**：`boost::filesystem`（93 处引用）+ `mmap`（`libreallive/filemap.cc`）与 SAF 的 Uri/fd 模型不相容，需要引入虚拟文件系统层。
   - **音频**：`SDL_mixer`（`Mix_OpenAudio` / `Mix_HookMusic` 回调模型）需改造成 Oboe 流 + 自研免锁混音器。
5. 中等成本：`boost::serialization` 存档格式必须保持兼容（Boost 文本归档 + zlib），不能换成 protobuf/JSON。
6. 低风险可直接复用：`encodings/`（cp932/936/949 纯查表）、`effects/`、`long_operations/`、`base/notification_*`、`machine/rloperation/`。
7. `vendor/` 中 luabind、gtest、gmock、GLEW、pygame 对主二进制不是必需（luabind 仅服务于可选的 `lua_rlvm` 测试工具），可裁剪掉约 5 万行。
8. GTK 与 Cocoa 壳层全部丢弃；Android 侧用 Compose 重写，`ACTION_OPEN_DOCUMENT_TREE` 替代 `RLVMInstance::SelectGameDirectory()`。

## 2. 分析方法

全过程只读，无编译、无写入。手段：

- `rg -o` 统计头文件依赖与符号命中次数；
- `rg -l` 按模块归类依赖分布；
- `Get-Content` 读取关键接口头文件与入口实现；
- `Measure-Object -Line` 逐文件统计行数。

关键结论均记录在第 12 节证据索引中。

## 3. 源码规模

| 区域 | 文件数 | 行数 | 说明 |
| --- | --- | --- | --- |
| `src/` | 393 | 62,092 | 引擎本体，本项目的移植对象 |
| `vendor/` | 345 | 124,248 | 第三方/可选依赖，大部分可裁剪 |

`src/` 各模块分布：

| 目录 | 文件 | 行数 | 职责 | 平台相关度 |
| --- | --- | --- | --- | --- |
| `libreallive/` | 23 | 4,362 | SEEN.TXT / Gameexe.ini 解析、字节码、压缩、文件映射 | 低（仅 filemap 的 mmap） |
| `machine/` | 36 | 6,527 | RLMachine 虚拟机、内存模型、栈帧、序列化 | 低（仅存档路径） |
| `machine/rloperation/` | 8 | 2,466 | 指令操作数类型系统（模板） | 无 |
| `modules/` | 80 | 10,930 | 约 50 个指令模块（grp/obj/msg/sys/koe 等） | 无（经 System 间接访问平台） |
| `systems/base/` | 101 | 16,893 | 平台抽象层 + 图形对象 / 文本窗口 / DC 管理 | 高（接口层） |
| `systems/sdl/` | 32 | 5,226 | SDL 1.2 后端：渲染 / 音频 / 事件 / 文本 | 极高（全部重写） |
| `platforms/gcn/` | 35 | 2,243 | guichan 实现的原生菜单与 SYSCOM 对话框 | 高 |
| `platforms/gtk/` | 4 | 367 | Linux GTK 壳层 + `main()` | 丢弃 |
| `platforms/osx/` | 7 | 605 | Cocoa 壳层 + SDLMain | 丢弃 |
| `effects/` | 12 | 1,763 | 转场特效（淡入 / 擦拭 / 滚动） | 无 |
| `long_operations/` | 14 | 1,772 | 长操作状态机（文本输出 / 选择 / 等待 / 缩放） | 无 |
| `encodings/` | 12 | 6,563 | CP932 / CP936 / CP949、半角全角、西文转换 | 无 |
| `utilities/` | 17 | 1,709 | 文件、字体查找、字符串、日期工具 | 中（font / file） |
| `base/` | 11 | 648 | 观察者 / 通知基础设施 | 无 |

`vendor/` 主要成分（可裁剪性判断依据）：

| 组件 | 文件 | 行数 | 是否需要 |
| --- | --- | --- | --- |
| GLEW | 6 | 27,902 | 不需要（`src/` 未 include，走 `<SDL/SDL_opengl.h>`） |
| gtest + gmock | 66 | 43,872 | 仅单元测试，阶段 6 再考虑 |
| guichan | 113 | 21,545 | 仅 `platforms/gcn` 菜单使用 |
| luabind | 99 | 11,485 | 仅 `lua_rlvm` 测试工具使用 |
| SDL_image | 18 | 5,824 | 仅窗口图标 `IMG_Load` + 图片解码 |
| SDL_mixer | 21 | 6,529 | 音频核心，需替换 |
| SDL_ttf | 4 | 2,045 | 字体渲染，需替换 |
| xclannad | 9 | 3,076 | NWK/KOE Ogg 语音解码，源码中参与链接 |
| utf8cpp | 5 | 795 | 需要，纯头文件 |

## 4. 分层架构与运行主循环

依赖方向自上而下，平台细节全部下沉到 `System` 抽象之下：

```
platforms/{gtk,osx}        桌面壳层（main + 原生对话框）    → 丢弃，Android 用 Compose
machine/rlvm_instance.cc   引擎装配 + 主循环                → 改 1 处硬编码
        │
machine/ + modules/ + long_operations/ + effects/           → 保留原样
        │  通过 machine.GetSystemObj<T>() 拿平台对象
systems/base/              System / GraphicsSystem / SoundSystem / EventSystem /
                           TextSystem / Surface / Platform   → 接口层，基本保留
        │
systems/sdl/               SDL 1.2 实现                      → 整体替换为 systems/android/
platforms/gcn/             guichan 菜单                      → 待定（Compose 重写或保留）
```

`RLVMInstance::Run()`（`src/machine/rlvm_instance.cc`）的实际装配顺序：

1. 定位 `Gameexe.ini` 与 `Seen.txt`，校验不是 AVG32 / Siglus 引擎；
2. 构造 `Gameexe`，写入 `__GAMEPATH`；
3. `libreallive::Archive arc(seenPath, gameexe("REGNAME"))` —— 此处 `Archive` 走 `Mapping`（mmap）读场景；
4. **`SDLSystem sdlSystem(gameexe);`** —— 唯一的硬编码后端实例化点；
5. `RLMachine rlmachine(sdlSystem, arc); AddAllModules(); AddGameHacks();`
6. `FindFontFile(sdlSystem)` —— 强制要求存在 `msgothic.ttc` 或回退字体，否则抛 `UserPresentableError`；
7. `GCNPlatform`（guichan）注册为 `System::platform()`；
8. `Serialization::loadGlobalMemory()`；
9. 主循环：`while (!rlmachine.halted())` → `sdlSystem.Run(machine)`（事件 + 重绘）→ 以 10ms 时间片批量执行字节码 → `event().Wait(sleep)` 让出 CPU；
10. 退出后 `Serialization::saveGlobalMemory()`。

结论：引擎从不直接调用 SDL，全部经抽象类。**移植的关键是把 4、7 两处的具体类换成 Android 实现，其余逻辑保持不动。**

## 5. 平台抽象接口清单（Android 后端必须实现）

| 抽象类 | 头文件 | 纯虚方法 | 关键方法 |
| --- | --- | --- | --- |
| `System` | `systems/base/system.h` | 6 | `Run` / `graphics` / `event` / `gameexe` / `text` / `sound` |
| `GraphicsSystem` | `systems/base/graphics_system.h` | 10 | `BeginFrame` / `EndFrame` / `EndFrameToSurface` / `AllocateDC` / `GetDC` / `GetHaikei` / `BuildSurface` / `BuildColourFiller` / `FreeDC` / `SetMinimumSizeForDC` |
| `SoundSystem` | `systems/base/sound_system.h` | 18 | `Bgm{Play,Stop,Pause,UnPause,FadeOut,Status,Name,Looping}` / `Wav{Play,Stop,StopAll,FadeOut,Playing}` / `PlaySe` / `HasSe` / `KoePlaying` / `KoeStop` / `KoePlayImpl` |
| `EventSystem` | `systems/base/event_system.h` | 12 | `ExecuteEventSystem` / `GetTicks` / `Wait` / `ShiftPressed` / `CtrlPressed` / `GetCursorPos`×2 / `FlushMouseClicks` / `TimeOfLastMouseMove` / `InjectMouse{Movement,Down,Up}` |
| `TextSystem` | `systems/base/text_system.h` | 2 | `GetTextWindow` / `GetCharWidth` |
| `Surface` | `systems/base/surface.h` | 10 | `Fill`×2 / `ToneCurve` / `Invert` / `Mono` / `ApplyColour` / `GetSize` / `GetDCPixel` / `Clone` |
| `Platform` | `systems/base/platform.h` | 4 | `Run` / `ShowNativeSyscomMenu` / `InvokeSyscomStandardUI` / `ShowSystemInfo` |

这些类的非纯虚部分（DC 分配、帧计数、文本排版、转场状态机）都已有跨平台实现，集中在 `systems/base/*.cc`，Android 后端只需补齐上表方法。

## 6. 桌面依赖清单（统计范围 = `src/`）

| 依赖 | 命中量 | 主要使用者 | 用途 |
| --- | --- | --- | --- |
| `boost::filesystem` | 93 处；头文件 `path.hpp`×17、`fstream.hpp`×12、`filesystem.hpp`×11、`operations.hpp`×10 | `systems/base`（44 个文件）、`machine`、`module_sys_save`、`gameexe`、`utilities/file` | 路径拼接、存在性判断、目录递归、文件流 |
| `boost::serialization` + `boost::archive` | 81 + 82 处 | `machine/serialization_{local,global}.cc`、`rlmachine`、`stack_frame`、`memory`、`save_game_header`、`lazy_array`、`dynamic_bitset_serialize` | 存档序列化（文本归档） |
| `boost::iostreams`（filtering_stream + zlib filter） | 18 处 | `machine/serialization_{local,global}.cc` | 存档 zlib 压缩 |
| `boost::algorithm/string`、`tokenizer`、`format`、`program_options`、`date_time`、`dynamic_bitset`、`scoped_ptr`、`shared_ptr`、`checked_delete`、`iterator_facade` | 合计约 50 处 | 分散 | 字符串处理、时间戳、位图内存、智能指针 |
| SDL 1.2（`<SDL/SDL.h>`） | `systems/sdl` 28 个文件、`platforms/gcn` 4、`gtk` 1、`osx` 1 | 全部平台功能 | 窗口 / 事件 / 音频 / 文本 |
| `<SDL/SDL_opengl.h>` | 8 个文件 | `sdl_graphics_system`、`texture`、`sdl_surface`、`shaders`、`sdl_text_window`、`sdl_colour_filter`、`sdl_utils` | OpenGL 1.x 固定管线 + ARB 扩展 |
| SDL_image | `IMG_Load` 1 处 | `sdl_graphics_system.cc`（窗口图标） | 图片解码 |
| SDL_mixer | 约 19 个 `Mix_*` 符号 + `Mix_OpenAudio` / `Mix_HookMusic` | `sdl_sound_system.cc`、`sdl_music.cc`、`sdl_sound_chunk.cc` | BGM / SE / 语音混音 |
| SDL_ttf | 约 13 个 `TTF_*` 符号 | `sdl_text_system.cc`、`sdl_text_window.cc` | 字体渲染（`TTF_RenderUTF8_Blended`） |
| libvorbis（`vorbis/vorbisfile.h`） | 2 个文件 | `systems/base/ovk_voice_sample.cc` | OVK 语音包内 Ogg 解码 |
| gettext / libintl | 1 个头文件（`ENABLE_NLS` 宏控制） | `utilities/gettext.h`，全项目 `_()` | 界面文案本地化 |
| guichan | 14 个文件 | `platforms/gcn` | 原生菜单 / 对话框 / 按钮 |
| luabind + Lua 5.1 | 仅 `test/script_machine` | `SConscript.luarlvm` | **仅测试工具 `lua_rlvm`，主程序不使用** |
| GTK+-2.0 | `platforms/gtk` | `rlvm.cc`、`gtk_rlvm_instance.cc` | Linux 壳层对话框 |
| `mmap`/`munmap`/`stat`/`fopen` | `libreallive/filemap.cc` | 场景文件映射 | 零拷贝读取 SEEN.TXT |
| GL / GLU | 链接期 `LIBS=["GL","GLU"]` | `SConscript.gtk` | 桌面 OpenGL |

注：`utilities/find_font_file.{h,cc}` 额外使用 `getenv` 探测 `$HOME` 与游戏目录，用于定位 `msgothic.ttc`。

## 7. 需重写 / 需保留

**必须新增或重写**

- `systems/android/`：`AndroidSystem`、`AndroidGraphicsSystem`、`AndroidEventSystem`、`AndroidTextSystem`、`AndroidSoundSystem`、`AndroidSurface`、`AndroidColourFilter`（替代 `systems/sdl/` 全部 32 个文件）。
- 渲染管线：`texture.cc`（24,877 字节）、`sdl_surface.cc`（27,902 字节）、`shaders.cc`、`sdl_graphics_system.cc`（21,574 字节）需从固定管线改写为 GLES 3.0（VBO + shader + `GL_TEXTURE_EXTERNAL_OES` 不需要，但需处理 NPOT、`glBlendEquation`、多纹理）。
- 音频：`audio-engine.cpp` 替代 `sdl_sound_system.cc` + `sdl_music.cc` + `sdl_sound_chunk.cc`（共约 2.7 万字节）。
- 文本：FreeType 直连替代 `sdl_text_system.cc` + `sdl_text_window.cc`（SDL_ttf 只是 FreeType 的薄封装）。
- 文件访问：新增 `VirtualFileSystem` 抽象，替代 `boost::filesystem` 的目录遍历与 `filemap.cc` 的 mmap。
- 壳层：Compose UI 替代 `platforms/gtk` + `platforms/osx`；`SelectGameDirectory` / `ReportFatalError` / `AskUserPrompt` 改由 JNI 回调 Kotlin。
- 平台对话框：`platforms/gcn`（guichan 菜单）需用 Compose 重写，或自写 Android 版 guichan 后端。
- 构建系统：SCons → CMake（`SConstruct` 357 行 + `SConscript` 236 行 + `site_scons/site_tools/rlvm.py`）。

**必须保留原样（不改语义）**

- `libreallive/`：字节码、表达式、压缩、场景解析（`filemap.cc` 只改 I/O 实现，不改解析逻辑）。
- `machine/` 与 `machine/rloperation/`：虚拟机指令语义、内存模型、栈帧。
- `modules/`：约 50 个指令模块的实现。
- `encodings/`：CP932/936/949 转换表。
- `effects/`、`long_operations/`、`base/`。

## 8. 关键风险

| 编号 | 等级 | 风险 | 说明与对策 |
| --- | --- | --- | --- |
| R1 | 阻塞 | SAF 与路径式文件系统不相容 | 引擎使用 `boost::filesystem::path` 拼接、`exists()`、以及 `System::BuildFileSystemCache` 递归扫描 `#FOLDNAME` 目录；`libreallive/filemap.cc` 用 `open()`+`mmap()` 映射 SEEN.TXT。SAF 只给 Uri + fd。对策：定义 `VirtualFileSystem`（open/read/stat/enumerate），Android 侧经 JNI 回调 Kotlin 的 `DocumentFile`/`ContentResolver`；场景文件退化为全量读入内存（放弃 mmap）。同时保留 `CorrectPathCase` 的大小写不敏感查找语义。 |
| R2 | 阻塞 | OpenGL ES 3.0 不兼容固定管线 | 后端使用 `glBegin/glEnd/glVertex2i/glMatrixMode/glActiveTextureARB/glCreateProgramObjectARB/glUniform*ARB` 等桌面专有 API。对策：重写为 GLES3（`glCreateProgram` + VBO + `glBlendEquation`）。上游已有 `shaders.cc` 与 `object.frag` 的着色器化雏形，可复用其设计。 |
| R3 | 高 | 音频回调线程约束 | 现模型在 SDL 音频线程内做混音，配合 `SDL_LockAudio`。Oboe 回调禁止分配内存、加锁、I/O、休眠。对策：预分配语音缓存（复用 `voice_cache.cc` 机制）+ 免锁环形缓冲，解码放在工作线程。 |
| R4 | 高 | 存档格式兼容 | `serialization_{local,global}.cc` 使用 Boost 文本归档 + 自定义 zlib filter。若需读取既有 PC 存档，必须保持该格式，不可换用 JSON/protobuf。同时需确认 Boost.Serialization 在 arm64-v8a / armeabi-v7a 交叉编译通过。 |
| R5 | 中高 | `boost::filesystem` 替换工作量 | 93 处引用、44 个文件集中在 `systems/base`。对策：不逐个替换调用点，先用 `std::filesystem`（NDK r26 的 libc++ 已完整支持）顶掉 `boost::filesystem` 以获得可编译基线，再在 T2.2 引入 SAF 语义。 |
| R6 | 中 | 字体依赖 | `RLVMInstance::Run` 强制要求 `msgothic.ttc` 或回退字体。商业字体不可分发。对策：改为读取 Android 系统日文字体（Noto Sans CJK）或打包 OFL 授权字体（如 IPAex Gothic），并让 `FindFontFile` 走 Android 字体提供者。 |
| R7 | 中 | gettext / NLS | Android NDK 无 libintl。对策：`ENABLE_NLS=0` 走 `utilities/gettext.h` 的内联空实现；注意该头文件同时 `using boost::format`，`boost/format` 依赖仍在。 |
| R8 | 中 | guichan 菜单系统 | `platforms/gcn` 35 个文件依赖 guichan + SDL + SDL_image，SDK 不提供。对策：用 Compose 重写 SYSCOM 菜单/存档对话框，C++ 侧只暴露 syscom 状态；需在 T3.2 前定案。 |
| R9 | 低中 | luabind / Lua 5.1 | 仅 `lua_rlvm` 测试工具需要。对策：Android 侧不编入，可直接省去 `vendor/luabind` 11,485 行与 `test/` 目录。 |
| R10 | 低中 | 线程模型与 GL 上下文 | 现主循环在单线程内完成「事件 → 逻辑 → GL 提交」。Android 的 `GLSurfaceView` 自带渲染线程，若引擎逻辑跑在另一线程则需引入命令队列，确保 GL 调用只在 GL 线程。建议首版让引擎逻辑跑在 GL 线程（与 `GameRenderer.onDrawFrame` 同线程），以最小改动换取正确性。 |

## 9. 目标 Android 文件映射建议

| 上游路径 | Android 目标 | 处理方式 |
| --- | --- | --- |
| `src/systems/sdl/*` | `app/src/main/cpp/android/` | 整体重写（GLES3 + Oboe + FreeType） |
| `src/systems/base/*` | 直接编入 `librlvm.so` | 保留，仅改文件访问与字体查找 |
| `src/platforms/gcn/*` | 弃用 | 由 Compose 菜单替代 |
| `src/platforms/{gtk,osx}/*` | 弃用 | 由 `MainActivity` + Compose 替代 |
| `src/machine/rlvm_instance.cc` | 改造为 `native-bridge.cpp` 调用 | 去掉 GTK/Cocoa 依赖，保留装配逻辑 |
| `src/libreallive/filemap.cc` | `android-fs.cpp` | mmap 改为全量读入 + 虚拟文件系统 |
| `src/utilities/file.cc` | `android-fs.cpp` | `boost::filesystem` → 虚拟文件系统 |
| `src/machine/serialization_*.cc` | 编入 `librlvm.so` | 保留 Boost 归档格式，IO 换成虚拟文件系统 |
| `SConstruct` / `SConscript` | `app/src/main/cpp/CMakeLists.txt` | 重写为 CMake + NDK |
| `vendor/{SDLK_*,guichan,luabind,gtest,gmock,GLEW}` | 不编入 | 裁剪 |
| `vendor/{utf8cpp,xclannad,lru_cache}` | 编入 | 纯源码，直接参与链接 |

## 10. 改写优先级表

优先级：P0 = 不解决就无法出包；P1 = 影响可运行性；P2 = 影响体验；P3 = 可延后。

| 模块 | 桌面依赖 | Android 替代方案 | 改写优先级 |
| --- | --- | --- | --- |
| `machine/rlvm_instance.cc` | Boost.Filesystem、SDLSystem 硬编码、GTK/Cocoa 对话框 | 抽出装配函数，经 JNI 由 Kotlin 传入游戏目录与字体；`System` 实例换成 `AndroidSystem` | P0 |
| `systems/sdl`（图形） | SDL 1.2 视频、`<SDL/SDL_opengl.h>`、OpenGL 1.x + ARB | GLSurfaceView + EGL + OpenGL ES 3.0，VBO + shader | P0 |
| `systems/sdl`（事件） | SDL 事件循环、`SDL_GetTicks`、`SDL_Delay` | Android 触摸/按键分发 + `clock_gettime` + 帧回调 | P0 |
| `libreallive/filemap.cc` | `open()` / `mmap()` | 全量读入 + `VirtualFileSystem` | P0 |
| `utilities/file.cc`、`systems/base/system.cc` | `boost::filesystem` 遍历/存在性判断 | `std::filesystem`（过渡）→ SAF 虚拟文件系统 | P0 |
| `machine/serialization_{local,global}.cc` | Boost.Archive、Boost.Iostreams/zlib | 保留格式；IO 换虚拟文件系统，zlib 沿用 | P0 |
| `systems/sdl`（音频） | SDL_mixer（`Mix_OpenAudio` / `Mix_HookMusic`） | Oboe 低延迟流 + 自研免锁混音器 | P1 |
| `systems/base/ovk_voice_sample.cc` | libvorbis | 保留 libvorbis（NDK 预编译）或换 stb_vorbis | P1 |
| `systems/sdl`（文本） | SDL_ttf | 直接调用 FreeType，字模缓存替换 SDL_ttf 接口 | P1 |
| `utilities/find_font_file.{h,cc}` | `getenv` + 文件系统探测 | Android 字体提供者 / 打包 OFL 字体 | P1 |
| `platforms/gcn` | guichan + SDL + SDL_image | Compose 重写 SYSCOM 菜单与存档对话框 | P2 |
| `systems/sdl/shaders.cc` | ARB `*ObjectARB`、`glCreateProgramObjectARB` | GLES3 原生 `glCreateProgram` + GLSL ES | P1 |
| `utilities/gettext.h` | libintl | `ENABLE_NLS=0` 空实现 | P2 |
| `encodings/` | 无 | 直接编入 | P3（零改动） |
| `effects/`、`long_operations/`、`base/` | 无 | 直接编入 | P3（零改动） |
| `modules/`、`machine/` | 无（经 `System` 间接） | 直接编入 | P3（零改动） |
| `vendor/luabind`、`test/` | Lua 5.1 | 不编入 | P3 |
| `vendor/{gtest,gmock}`、`SConscript.test` | 无 | 阶段 6 用 Android 原生测试替代 | P3 |

## 11. 与 task.md 阶段计划的差异建议

1. T1.2 原文「用 Oboe 替代 SDL 音频，NDK filesystem 替代 Boost.Filesystem」粒度偏粗。建议拆成两步：
   - T1.2a：`boost::filesystem` → `std::filesystem`，目标是**先获得可编译基线**，此时仍假定能用普通路径访问；
   - T2.2：再引入 SAF 虚拟文件系统，把路径访问换成 Uri/fd。
2. T4.2 原文「适配 GLSurfaceView.Renderer，管理 EGL 上下文」低估了工作量：真正的成本在于把 OpenGL 1.x 立即模式改为 GLES 3.0 管线，涉及 `texture.cc` / `sdl_surface.cc` / `sdl_graphics_system.cc` / `shaders.cc` 四个大文件。建议在阶段 4 单独列为「渲染管线重写」任务，并预留最多工作量。
3. 建议把「可编译基线」作为阶段 1 的硬门禁：先让 `librlvm.so` 在 NDK 下编译通过（`encodings` + `machine` + `modules` + `systems/base`），再谈平台功能。这比按 T1.1→T1.4 顺序推进更安全。
4. 建议明确 `System` 后端的引入方式为「增量 bring-up」：先实现 `AndroidSystem` 的 `event()` 与 `Run()` 让主循环转起来，再逐步补 `graphics()`、`text()`、`sound()`。

## 12. 关键证据索引

| 事实 | 证据位置 |
| --- | --- |
| 唯一硬编码后端实例化点 | `src/machine/rlvm_instance.cc` — `SDLSystem sdlSystem(gameexe);` |
| 强制字体校验 | `src/machine/rlvm_instance.cc` — `FindFontFile(sdlSystem)` 后 `throw rlvm::UserPresentableError` |
| 主循环 10ms 时间片 | `src/machine/rlvm_instance.cc` — `rlmachine.ExecuteNextInstruction()` / `real_sleep_time = 10 - ...` |
| `System` 抽象接口 | `src/systems/base/system.h` |
| 后端组合（4 个子系统） | `src/systems/sdl/sdl_system.h` |
| 后端生命周期入口 | `src/systems/sdl/sdl_system.cc` — `SDL_Init(SDL_INIT_VIDEO)` |
| 固定管线渲染 | `src/systems/sdl/texture.cc`、`sdl_graphics_system.cc` — `glBegin(GL_QUADS)` |
| GL 头依赖 | `src/systems/sdl/texture.h` 等 8 个文件 — `#include <SDL/SDL_opengl.h>` |
| 音频设备打开 | `src/systems/sdl/sdl_sound_system.cc` — `Mix_OpenAudio` / `Mix_HookMusic` |
| 字体渲染 | `src/systems/sdl/sdl_text_system.cc` — `TTF_OpenFont` / `TTF_RenderUTF8_Blended` |
| mmap 场景读取 | `src/libreallive/filemap.cc` — `mmap(0, len, PROT_READ, MAP_SHARED, fp, 0)` |
| 存档 zlib 压缩 | `src/machine/serialization_{local,global}.cc` — `boost::iostreams::zlib_compressor` |
| OVK 语音 Ogg 解码 | `src/systems/base/ovk_voice_sample.cc` — `#include <vorbis/vorbisfile.h>` |
| luabind 仅测试用 | `SConscript.luarlvm` — `lua_rlvm` 目标，注释「end users don't use this binary」 |
| GTK 壳层与 GLU 链接 | `SConscript.gtk` — `pkg-config gtk+-2.0`、`LIBS=["GL","GLU"]` |
| 编译标准 C++11 | `SConscript` — `CXXFLAGS = [..., "-std=c++11"]` |
| 主库源文件清单 | `SConscript` — `librlvm_files`（约 160 个 .cc） |
| SDL 后端源文件清单 | `SConscript` — `libsystemsdl_files`（16 个 .cc，含 `vendor/pygame/alphablit.cc`） |

## 13. 待人工确认项

1. **菜单系统**：`platforms/gcn` 的 guichan 菜单用 Compose 重写，还是保留 guichan 自写 Android 后端？前者动 Kotlin、后者动 C++，工作量与风险差别很大。
2. **字体策略**：是否接受打包 OFL 授权日文字体（约 4–6 MB），还是仅使用系统 Noto Sans CJK？
3. **存档兼容**：是否需要读写既有 PC 版存档？若是，Boost.Serialization 归档格式与 zlib 必须原样保留；若否，可自由换格式。
4. **Boost 版本**：是否允许使用 Boost 1.74+（旧版 Boost.Filesystem/Serialization 在 arm64 NDK 上有已知编译问题）？
5. **目录约定**：本报告假定 `docs/`、`dev-log/` 落在工作区根目录 `rlvm-release-0.14/` 下；确认后按 task.md 建议结构补齐 `app/`、`AGENTS.md`、`TASKS.md`。
6. **上游裁剪策略**：是否接受为 Android 目标维护一份"裁剪后的源码树"（去掉 gtk/osx/gcn/luabind），还是尽量保持上游目录完整、仅靠 CMake 排除？
