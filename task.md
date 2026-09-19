RLVM Android 封装工程 — 给 LLM/Codex 的任务计划
本文件可直接作为 TASKS.md 交给 Codex Agent 执行。
Agent 每次启动后必须先读取 AGENTS.md、TASKS.md 和 dev-log/ 中最近日志，再按任务 ID 顺序推进。

0. 工程元信息
项目	内容
工程名	rlvm-android
最终目标	将 RLVM C++ 核心封装为现代 Android APK/AAB，支持 Android 12+
最低版本	minSdk = 31
目标版本	targetSdk = 35
支持 ABI	arm64-v8a、armeabi-v7a
UI 框架	Jetpack Compose + Material 3
原生构建	Android NDK r26+、CMake、C++17
JNI 策略	显式注册 JNIRegisterNatives
文件访问	Storage Access Framework，禁止直接 File 路径
音频	Oboe / AAudio
渲染	OpenGL ES 3.0 + GLSurfaceView
协议	GPLv3，项目必须开源
Agent 框架	Codex
模型提供方	DeepSeek API
模型路由	日常：deepseek-flash；复杂：deepseek-v4-pro
1. LLM 角色与总约束
你是负责 RLVM Android 封装的 Android NDK / C++ / Kotlin / Compose 专家 Agent。你必须：

先读后写：修改任何文件前，先读取相关源码、CMake、Gradle、Manifest 和错误日志。

最小修改：只改必要文件，不重构 RLVM 核心逻辑。

每改必验：每次修改后运行对应构建命令，并把结果写入日志。

显式 JNI：所有 native 方法必须用 JNIRegisterNatives 显式注册，不用动态查找。

SAF 优先：Android 12+ 外部文件访问必须通过 SAF 和 DocumentFile。

禁止事项：

不下载、不打包任何商业游戏资源；

不使用 READ_EXTERNAL_STORAGE 等宽泛存储权限；

不在音频回调中分配内存、做 I/O、加锁、休眠；

不引入 GPLv3 不兼容的专有库；

不修改 libreallive、machine 的脚本解析核心逻辑，除非明确要求。

输出格式：

修改代码：输出 diff 或完整文件；

分析任务：输出 Markdown 报告；

编译修复：输出“原因 + 修改文件 + 验证命令”；

每次结束：写入 JSONL 任务日志。

2. 工作流循环
每个任务按以下循环执行：

text
读取任务 -> 读取上下文 -> 制定计划 -> 修改/生成代码
-> 本地构建 -> 回传日志 -> 修复 -> 记录日志 -> Git commit
人工检查点：阶段结束、涉及核心逻辑、Manifest 权限、GPL 合规时必须暂停，等待用户确认。

模型切换：简单生成和编译修复用 deepseek-flash；架构设计、JNI、CMake 依赖、音频/渲染、疑难 bug 用 deepseek-v4-pro。

上下文管理：每完成一个任务，把决策写入 docs/DECISIONS.md，把变更写入 dev-log/，然后在上下文使用率约 35% 时做 compaction。

3. 建议仓库结构
text
rlvm-android/
├── AGENTS.md
├── TASKS.md
├── docs/
│   ├── DECISIONS.md
│   └── ARCHITECTURE.md
├── dev-log/
│   └── *.jsonl
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/.../
        │   ├── MainActivity.kt
        │   ├── GameViewModel.kt
        │   ├── NativeBridge.kt
        │   ├── DirectoryPicker.kt
        │   ├── GameService.kt
        │   └── GameRenderer.kt
        └── cpp/
            ├── CMakeLists.txt
            ├── rlvm/                 # RLVM Source code (zip) 解压于此
            ├── native-bridge.cpp
            ├── android-fs.cpp
            ├── audio-engine.cpp
            └── gl-renderer.cpp
4. 任务清单
阶段 0：认知与仓库初始化
ID	任务	LLM 动作	输出	验收	模型
T0.1	只读分析 RLVM 源码	读取源码目录，识别核心模块、桌面依赖、需重写部分	docs/ARCHITECTURE.md	模块列表完整，依赖清单准确	v4-pro
T0.2	生成 Android 项目骨架	生成 Gradle KTS、CMake、MainActivity、Manifest	可编译空项目	./gradlew assembleDebug 通过	flash
T0.3	写入 AGENTS.md 与 TASKS.md	根据本计划生成项目级指令	AGENTS.md、TASKS.md	Codex 启动时能自动读取	flash
阶段 1：原生层编译适配
ID	任务	LLM 动作	输出	验收	模型
T1.1	Android 化 CMakeLists	修改 RLVM CMake，设置 C++17、NDK、ABI、链接库	cpp/CMakeLists.txt	CMake 配置无错误	v4-pro
T1.2	替换桌面依赖	用 Oboe 替代 SDL 音频，NDK filesystem 替代 Boost.Filesystem，处理 Ogg/Vorbis	依赖替换方案 + 代码	缺失依赖逐步减少	v4-pro
T1.3	编译迭代修复	根据本地错误日志逐条修复	可编译的 librlvm.so	externalNativeBuildDebug 通过	flash/v4-pro
T1.4	最小 JNI 验证	实现 stringFromJNI 或 nativeInit，验证桥接	native-bridge.cpp	App 能调用 native 并返回结果	flash
阶段 2：文件系统与 JNI 桥接
ID	任务	LLM 动作	输出	验收	模型
T2.1	JNI 接口设计	定义 Kotlin external 与 C++ 显式注册	NativeBridge.kt、native-bridge.cpp	方法可被调用，无动态查找	v4-pro
T2.2	AndroidFileSystem	通过 JNI 回调 Kotlin DocumentFile 读取 SAF URI	android-fs.cpp/.h	能读取 SEEN.txt	v4-pro
T2.3	触摸事件映射	单指拖动=相对移动，单指点击=左键，双指点击=右键	Kotlin 触摸监听 + JNI	游戏内光标可移动、点击	flash
T2.4	引擎生命周期	initEngine/start/pause/resume/shutdown	JNI + Kotlin 封装	能启动、暂停、恢复、退出	v4-pro
阶段 3：Compose UI 与 SAF
ID	任务	LLM 动作	输出	验收	模型
T3.1	SAF 目录选择器	ACTION_OPEN_DOCUMENT_TREE + 持久化权限	DirectoryPicker.kt	重启后仍可访问目录	flash
T3.2	游戏库与设置 UI	Compose 列表、添加游戏、设置页、ViewModel + StateFlow	MainScreen.kt、GameViewModel.kt	界面可交互	flash
T3.3	GLSurfaceView 集成	用 AndroidView 嵌入 GLSurfaceView，处理层级	GameScreen.kt	游戏画面能显示	v4-pro
阶段 4：音频与渲染
ID	任务	LLM 动作	输出	验收	模型
T4.1	Oboe 音频	低延迟输出流，回调中无阻塞，桥接 RLVM PCM	audio-engine.cpp	语音播放正常，无爆音	v4-pro
T4.2	OpenGL ES 渲染	适配 GLSurfaceView.Renderer，管理 EGL 上下文	gl-renderer.cpp、GameRenderer.kt	画面正常，无花屏	v4-pro
T4.3	同步与性能	音画同步、帧率稳定、避免跨线程 GL 调用	优化补丁	视觉小说场景稳定 60fps	v4-pro
阶段 5：Android 12+ 系统适配
ID	任务	LLM 动作	输出	验收	模型
T5.1	前台服务	声明 foregroundServiceType="mediaPlayback"，仅前台启动	GameService.kt、Manifest	后台运行不被杀	flash
T5.2	组件导出	为含 intent-filter 组件添加 android:exported	Manifest 修正	Android 12+ 安装无错	flash
T5.3	PendingIntent 与休眠	添加 FLAG_IMMUTABLE，处理权限被重置	代码修正	无系统级崩溃	flash
阶段 6：测试、优化与发布
ID	任务	LLM 动作	输出	验收	模型
T6.1	单元/仪器测试	为 JNI、文件系统、ViewModel 生成测试	测试文件	测试通过	flash
T6.2	Profiler 优化	分析 CPU/内存/GPU 数据，给出优化	优化报告 + 补丁	帧率、内存达标	v4-pro
T6.3	发布配置	签名、AAB、GPLv3 说明、开源仓库	release/ 文档	可生成 AAB	flash
5. 给 LLM 的 Prompt 模板
5.1 分析任务
text
目标：分析 [模块/文件] 在 Android 上的适配需求
上下文：[粘贴源码目录或文件内容]
约束：minSdk=31，targetSdk=35，只支持 arm64-v8a/armeabi-v7a，文件访问必须用 SAF
输出格式：Markdown 表格，列为：模块 / 桌面依赖 / Android 替代方案 / 改写优先级
5.2 代码生成
text
目标：生成 [类/文件] 的 Android 实现
上下文：[相关文件路径和现有代码]
期望行为：[具体功能]
约束：[JNI 显式注册 / SAF / Oboe 回调禁止阻塞 / 不修改核心逻辑]
输出格式：完整文件内容 + 需要修改的 Gradle/CMake 片段
5.3 编译修复
text
目标：修复以下 Android NDK 编译错误
错误日志：[粘贴原文]
相关文件：[CMakeLists.txt 路径]
期望行为：只改必要配置，不修改 RLVM 业务逻辑
输出格式：原因 + diff + 验证命令
5.4 审查任务
text
目标：审查本次改动是否符合 Android 12+ 和 GPLv3 要求
上下文：[git diff]
检查项：SAF、前台服务类型、exported、PendingIntent、JNI 显式注册、无专有库
输出格式：问题列表 + 修复建议
6. 任务日志格式
每个任务写入 dev-log/<task-id>.jsonl：

jsonl
{"type":"task","id":"T1.3","phase":"阶段一","timestamp":"2026-09-20T10:30:00+08:00","goal":"修复 filesystem 编译错误"}
{"type":"prompt","model":"deepseek-flash","content":"编译报错：'filesystem' is not a member of 'std'..."}
{"type":"agent_output","content":"原因：NDK libc++ 需要 C++17。修改 CMakeLists.txt 添加 set(CMAKE_CXX_STANDARD 17)..."}
{"type":"local_verify","command":"./gradlew externalNativeBuildDebug","result":"该错误消失，出现新错误：Boost 缺失","status":"partial"}
{"type":"next","action":"将新错误日志发给 Agent，请求替换 Boost 依赖"}
7. 验收门禁
阶段结束必须满足：

./gradlew assembleDebug 成功；

APK 可安装到 Android 12+ 设备/模拟器；

SAF 选择目录后能读取 SEEN.txt 和 RealLive.exe；

游戏能启动、显示画面、播放语音；

暂停/恢复/退出正常；

后台前台服务不被杀；

Android 12+ 无 exported、PendingIntent、前台服务崩溃；

项目以 GPLv3 开源，未引入不兼容库。

8. 停止与升级条件
连续 3 次修复同一编译错误仍失败：暂停，请求人工介入。

涉及 RLVM 核心逻辑修改：暂停，等待用户确认。

保留 AGENTS.md、TASKS.md、最近错误日志和 docs/DECISIONS.md。