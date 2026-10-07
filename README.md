# RLVM for Android

把 [RLVM](http://rlvm.org/)（RealLive 虚拟机的开源重实现）封装成一个现代 Android
应用，让 RealLive 引擎的游戏能在 Android 上运行。

## 版本 / 下载

**最新：`v0.3.0`**（`versionCode 7`）——<https://github.com/Tooke258/rlvm-android/releases>

相对 v0.2.4，这一版把积压的**存档 / 相册 / 选择枝**与 **LBEX 棒球小游戏的一批真修复**
一起放出来：

- **击球方向不再系统性反向**：执行器 x87 出栈式算术（`DE` 组）的目的槽写死 `st(1)`，
  `i≠1` 时"值对、槽错"；修好后与原版 `PT00.dll` 逐位一致；
- **外野回传落点不再偏到身后**：补跑 DLL 的 CRT 静态构造（真实宿主 `LoadLibrary` 会跑）；
- **整片 UI 不再"先正确一瞬、随后归位默认"**：LBEX 的相册页码表落在 `intA[7200+n]`，
  而我们的存储区只有 2000 项、且有一处写死的 2000 访问上限；
- **菜单关掉后不再残留**；图标不再被按钮态覆盖成同一张 0 号图；
- **相册 Scene 回想不再卡死**（根因在自家音频层：淡出只降音量不停通道）、**剧情选择枝修复**。

完整说明见 [`docs/RELEASE-NOTES-v0.3.0.md`](docs/RELEASE-NOTES-v0.3.0.md)。

## 这是什么 / 不是什么

- **是**：RLVM 的 Android 移植层。上游引擎（`libreallive` / `machine` / `modules` /
  `systems/base`）保留原样，本仓库提供它需要的平台后端（图形、音频、文字、事件、
  文件系统），以及一个最小的 Android 壳层。
- **不是**：不含任何游戏本体、语音包、图片或脚本。它只读取你自己合法拥有的游戏
  目录，通过 Android 的 SAF（Storage Access Framework）授权访问。

## 支持的游戏

下表**照抄自上游 RLVM 的 README**，是上游在桌面平台（Linux/macOS）的验证结果。
本移植复用了它的引擎实现，理论上具备同样的兼容性，但**目前只在真机上验证过
Kud Wafter**（标题→正文、语音、BGM、存档、Config 均正常）。

| Japanese Edition Games | Status | English Fan Patch Status |
| ---------------------- | ------ | ------------------------ |
| Kanon Standard Edition | OK     | NDT's patch              |
| Air Standard Edition   | OK     | (None)                   |
| CLANNAD                | OK     | (Not supported)          |
| CLANNAD (Full Voice)   | OK     | Licensed                 |
| Planetarian CD         | OK     | Licensed                 |
| Tomoyo After           | OK     | (None)                   |
| Little Busters         | OK     | (Untested)               |
| Kud Wafter             | OK     | (None)                   |

| US Edition Games | Status    |
| ---------------- | --------- |
| Planetarian      | Works     |

其他作品**可能**可以运行——上游的实现已经足够完整，但上表只列出实际验证过的；
本移植尚未逐个验证。语音支持 KOE / NWK / OVK 归档，以及遵循
`<packnumber>/z<packnumber><sampleid>.ogg` 约定的 ogg 语音补丁。

遇到问题请把日志标签 `rlvm-stdout` / `rlvm-stderr` 的内容一并附上（上游把未实现的
操作码与指令异常都打到这两个流，本移植已把它们接到 logcat）。

### 本移植真机验证（Android，Redmi K40 游戏版 / Android 12）

| 作品 | 状态 |
| --- | --- |
| Kud Wafter（日文原版） | 已验证：标题 → 正文、语音、BGM、存档、Config |
| Little Busters! EX（**汉化版**） | 主要流程可用：对白、存档/读档、相册（含 Scene 回想）、剧情选择枝；小游戏可进入并推进（击球方向 / 回传落点已修） |

### 已知问题

* 小游戏**实体渲染**仍不完整（猫 / 球 / 打者有时不出或位置不对），练习选择菜单
  `PT_PR_*` 有细节缺失；
* 事件牌 / 公告牌（`PT_ANN*`、`PT_CALL*`）在特定状态下会卡在屏幕上 —— 面板有
  「事件牌：屏蔽/恢复」开关可手动清掉；
* `logic_hz` 与 `lb_minigame` 两项仍在 `rlvm-diag.txt`（见「诊断」）：**删掉该文件**
  会让小游戏逻辑帧率从标定值 37/s 回到默认 65.7/s；
* 上游 `Sys 457/2402/2502/1520/1521/366/801` 等操作码仍未实现。

## 许可证（重要）

RLVM 以 **GNU GPLv3（或更新版本）** 发布，本移植同样如此：见 `LICENSE`
（GPLv3 全文）与 `COPYING.TXT`（上游的第三方组件许可汇总）。

由于本仓库修改了部分上游文件，按 GPLv3 要求，**修改后的完整源码即本仓库本身**，
并保留了上游源码树（`rlvm-release-0.14/`）以便对照审查：

```bash
git diff b87ff44 HEAD -- rlvm-release-0.14
```

发行二进制（APK）同样受 GPLv3 约束：**分发 APK 时必须同时提供本仓库源码**。

## 构建

前置条件：

| 项 | 版本 / 路径 |
| --- | --- |
| JDK | 17 或更高（AGP 9 内置 Kotlin，不引入独立的 Kotlin 插件） |
| Android SDK | compileSdk 36、build-tools 36.0.0 |
| Android NDK | 28.2.13676358（r28c） |
| Boost | 1.92.0 源码树（**必须**，路径见下） |
| CMake | 3.22.1（SDK 自带即可） |

第三方依赖（FreeType / libogg / libvorbis）不进仓库，用脚本获取：

```powershell
powershell -File tools/setup_third_party.ps1
```

Boost 需要一份源码树。用环境变量、`-DBOOST_SOURCE_DIR=`，或在 gitignore 的
`local.properties` 里写 `boost.dir=` 指定：

```powershell
$env:BOOST_SOURCE_DIR = "<Boost 源码树>"
```

构建：

```powershell
.\gradlew.bat assembleDebug     # 调试包（额外含 x86_64，便于模拟器）
.\gradlew.bat assembleRelease   # 发布包（只有 arm64-v8a 与 armeabi-v7a）
```

发布签名：把 keystore 放在仓库根目录，并创建 `keystore.properties`：

```properties
storeFile=your-release.jks
storePassword=***
keyAlias=***
keyPassword=***
```

两者都在 `.gitignore` 里。**文件不存在时 release 自动回退到 debug 签名**，
因此克隆下来无需任何签名材料也能构建、安装、运行。

## 使用

1. 把游戏目录（含 `Gameexe.ini`、`Seen.txt` 等）拷到设备上任意位置；
2. 应用内点「选择游戏目录」，在系统选择器里授予该目录（授权会持久化）；
3. 点「运行 SAF 引擎」。界面默认只有画面与右侧悬浮球，点球展开控制面板与日志。

触摸操作（等价于鼠标）：

| 手势 | 等价于 | 用途 |
| --- | --- | --- |
| 单击 | **左键** | 推进对白、选择菜单项 |
| **长按** | **右键** | 打开游戏菜单（存档 / 读档 / 配置 / 退出等） |
| 滑动 | 移动鼠标 | 移动光标（悬停高亮、按钮命中判定） |
| 悬浮球 | —— | 展开 / 收起控制面板与日志；可拖动，松手自动贴边 |

> 游戏菜单在 RealLive 里由右键唤出，Android 没有右键，因此用**长按**代替（阈值
> 400ms）。这也是正常退出游戏的途径，否则只能依靠系统杀掉进程。

存档与设置写在应用外部文件目录的 `.rlvm/<REGNAME>/` 下（**不是**游戏目录，
因为 PC 版 RealLive 的原生存档格式与 RLVM 自己的格式不互通）：

```
/sdcard/Android/data/org.rlvm.android/files/.rlvm/KEY_<游戏名>/
    global.sav.gz     # 全局设置（Config）
    save000.sav.gz    # 存档槽
```

### 文本容器（语言）切换

译文数据常常放在**另一个文件**里（不是游戏目录里的 `SEEN.TXT`）。应用不要求你覆盖原始
文件，而是给一个显式开关：

1. 把容器文件放到游戏目录下（例如 `Seen-CN.TXT`）；
2. 面板里点「选择文本容器」，在弹窗里选它 —— 候选列表由应用自己列游戏目录，
   **不走系统文件选择器**（部分机型在选择大文件时会把系统文件界面搞崩）；
3. 「容器：开」时引擎读该容器，「容器：关」时读游戏目录里的 `Seen.txt`（原版）。

容器需要自带每个场景的编码声明（RLdev 元数据块里的 encoding 字节：`0` = CP932、
`1` = CP936/GBK），否则中文会被当作 CP932 解码成乱码。游戏目录里的 `Seen####.txt`
场景覆盖文件优先级高于容器（RealLive 补丁机制的既有语义）。

## 平台后端说明

| 模块 | 实现 |
| --- | --- |
| 图形 | `AndroidSurface`：CPU 像素表面 + GLES3 上屏；支持适配/贴边/拉伸四种缩放 |
| 音频 | AAudio（minSdk 31 起可用，不需要 Oboe）：解码线程 → 无锁环形缓冲 → 音频回调 |
| 文字 | FreeType + 系统 CJK 字体（不打包商业字体） |
| 输入 | 触摸 → 游戏帧坐标映射；长按 = 右键 |
| 文件 | 全部经 `GameFileSystem` 抽象：普通路径后端与 SAF 后端，读与写都在其内 |

细节与踩坑记录见 `docs/`：`PROGRESS.md`（交接文档）、`ARCHITECTURE.md`（上游源码
分析）、`DECISIONS.md`（关键取舍）、`TESTING.md`（真机测试流程）。

## 诊断

真机迭代不需要重新构建：把 `tools/rlvm-diag.sample.txt` 改名成 `rlvm-diag.txt`，
push 到应用外部文件目录即可调整观察参数（逐条指令追踪、图形栈转储、运行时长、
音频统计等）。日志标签：`rlvm-native` / `rlvm-audio` / `rlvm-gl` / `rlvm-font` /
`rlvm-graphics`，以及上游诊断输出 `rlvm-stdout` / `rlvm-stderr`。

**常规配置**是 [`tools/rlvm-diag.default.txt`](tools/rlvm-diag.default.txt)：

```ini
lb_minigame=1     # 关掉上游把 LB 棒球整段绕过去的 hack
logic_hz=37       # 逻辑帧节拍，按 PC 实测 37.5 逻辑帧/秒标定（只节流脚本，不压渲染）
```

**这两项仍在 diag 层** —— 删掉/清空 `rlvm-diag.txt` 会静默回退到默认行为
（小游戏逻辑帧率变快、且 LB 棒球会被跳过）。装包后若要长期稳定运行，请保留该文件。
其余取证开关（`patno_trace` / `gallery_probe` / `wipe_log` / `op_trace` …）默认全关，
需要时再临时覆盖。
