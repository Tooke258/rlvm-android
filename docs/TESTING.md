# 真机测试回路

> 建立于 2026-09-19。这是本项目的验证基线——所有功能改动都应能通过这套流程验证。

## 1. 测试设备

| 项 | 值 |
| --- | --- |
| 机型 | Redmi K40 游戏版（M2012K10C / `ares`） |
| adb serial | `if6lf67xkfs4ibf6` |
| Android | 12（SDK 31） |
| ABI | `arm64-v8a`、`armeabi-v7a` |
| GPU | Adreno 650（后续 GLES 渲染验证的目标机型） |

该设备恰好匹配 task.md 的 `minSdk = 31` 与两个目标 ABI，**不需要 x86_64**。debug 变体虽然也编 x86_64（为将来可能的模拟器测试预留），但真机测试只用 arm64-v8a。

## 2. 前提：命令需要提权

`adb` 与 `gradlew` 在 Codex 沙箱内都不工作：

- `adb` 需要写 `~/.android`，沙箱内只读，会直接崩溃（`Cannot mkdir '\.android'`）；
- Gradle 需要写 `~/.gradle/native`。

两者都必须用提权（`require_escalated`）执行。构建前要显式设置：

```powershell
$env:JAVA_HOME='E:\STM32cubeMX\jre'
$env:ANDROID_SDK_ROOT='E:\DEV\AndroidSdk'
```

## 3. 标准流程

```powershell
$adb='E:\DEV\AndroidSdk\platform-tools\adb.exe'
$s='if6lf67xkfs4ibf6'
$repo='C:\Users\tooke\Desktop\rlvm-release-0.14'

# 1) 确认设备在线
& $adb devices -l

# 2) 构建
Set-Location $repo; & .\gradlew.bat assembleDebug --no-daemon

# 3) 安装
& $adb -s $s install -r "$repo\app\build\outputs\apk\debug\app-debug.apk"

# 4) 清日志 → 启动 → 读取
& $adb -s $s logcat -c
& $adb -s $s shell am force-stop org.rlvm.android
& $adb -s $s shell am start -n org.rlvm.android/.MainActivity
Start-Sleep -Seconds 7
& $adb -s $s logcat -d -s rlvm-native:V '*:S'
& $adb -s $s logcat -d -s AndroidRuntime:E libc:F     # 崩溃检查
```

按需要截屏：

```powershell
& $adb -s $s shell screencap -p /sdcard/shot.png
& $adb -s $s pull /sdcard/shot.png "$repo\build\shot.png"
& $adb -s $s shell rm /sdcard/shot.png
```

### 设备侧诊断参数（不需要重新构建）

把诊断文件 push 到下面这个位置，再点「运行 SAF 引擎」即可生效；文件不存在时全部取缺省值。
模板见 `tools/rlvm-diag.sample.txt`：

```powershell
& $adb -s $s push build\rlvm-diag.txt /sdcard/Android/data/org.rlvm.android/files/rlvm-diag.txt
```

支持的键（详见模板文件）：

| 键 | 作用 |
| --- | --- |
| `trace=1` | 逐条指令追踪（上游 `set_tracing_on`），输出在 `rlvm-stderr` |
| `dump_graphics=1` | 运行结束时转储图形栈：每个对象的 src/dst 矩形、alpha、可见性 |
| `time_budget_ms=N` | 单次运行的执行时间片，缺省 3000 |
| `max_instructions=N` | 指令条数上限 |
| `frame_log_every=N` | 每 N 帧打一条帧日志，缺省 1（每帧） |

排查「跑了很多指令却什么都没发生」时，上游诊断输出是关键：

```powershell
& $adb -s $s logcat -d -s rlvm-native:V rlvm-stdout:V rlvm-stderr:V rlvm-graphics:V '*:S'
```

## 4. 测试数据

**不使用任何商业游戏文件。** 测试样本取自 RLVM 上游自带的测试数据（GPLv3 项目自带，且这些 SEEN 是上游开发者生成的最小夹具）：

| 用途 | 来源 | 部署位置 |
| --- | --- | --- |
| Gameexe | `rlvm-release-0.14/test/Gameexe_data/Gameexe.ini`（601 B） | `<应用外部目录>/probe/Gameexe.ini` |
| SEEN 归档 | `rlvm-release-0.14/test/Module_Mem_SEEN/cpyrng_0.TXT`（80,573 B） | `<应用外部目录>/probe/Seen.txt` |

应用外部目录是 `/sdcard/Android/data/org.rlvm.android/files/probe/`：

- 应用可读写且**不需要任何存储权限**（即便是 Android 11+ 的分区存储限制也不影响应用自己的目录）；
- `adb push` 可以直接写入；
- 卸载应用即清除，不会污染用户数据。

推送命令：

```powershell
$fx="$repo\rlvm-release-0.14\test"
& $adb -s $s shell mkdir -p /sdcard/Android/data/org.rlvm.android/files/probe
& $adb -s $s push "$fx\Gameexe_data\Gameexe.ini" /sdcard/Android/data/org.rlvm.android/files/probe/Gameexe.ini
& $adb -s $s push "$fx\Module_Mem_SEEN\cpyrng_0.TXT" /sdcard/Android/data/org.rlvm.android/files/probe/Seen.txt
```

### 关于 SEEN 夹具的一个坑

`Archive::ReadTOC()` 读取的是一张 `(offset, length)` 表，**offset 为 0 表示该索引不存在**，所以场景索引不一定从 0 开始。`cpyrng_0.TXT` 的索引是 **1**，不是 0。遍历 TOC 请用 `Archive::begin()/end()`，不要硬编码索引。

## 5. 已有验证记录

| 时间 | 验证内容 | 结果 |
| --- | --- | --- |
| 2026-09-19 | JNI 桥接 + RLVM 解析链路（T1.4） | 通过：Gameexe 解析、SEEN TOC（1 个场景，索引 1）、Scenario 构造（含解压）、无崩溃 |
| 2026-09-19 | 引擎装配 + 执行字节码（T2.4） | 通过：AndroidSystem 顶替 SDLSystem，执行 6 条指令后进入文本长操作，无崩溃 |
| 2026-09-19 | SAF 文件访问（T2.2） | 通过：SAF 目录列举、Gameexe.ini 整体读入、SEEN.TXT 经 fd + mmap 打开并解析出同样的 TOC |
| 2026-09-19 | `SEEN####.TXT` 场景覆盖（补丁机制） | 通过：两条后端 TOC 均变为 `[1,2]`，覆盖文件内容也被正确解析 |
| 2026-09-19 | 大小写不敏感解析 | 通过：夹具改为全小写（`gameexe.ini`/`seen.txt`/`seen0002.txt`）后，两条后端仍能按标准名找到文件 |
| 2026-09-19 | 游戏资源查找层（T2.2） | 通过：SAF 下 `FindFile(doesntmatter, g00)` 返回 `g00/doesntmatter.g00`，并可用 fd 打开；普通路径无回归 |

## 6. SAF 测试流程

SAF 的目录授权**必须由用户在系统选择器中完成一次**，无法用 adb 或代码绕过——这是 SAF 的设计。

```powershell
# 1) 把夹具放到用户可见、选择器够得到的目录
& $adb -s $s shell mkdir -p /sdcard/Download/rlvm-probe
& $adb -s $s push "$repo\build\probe-fixture\Gameexe.ini" /sdcard/Download/rlvm-probe/Gameexe.ini
& $adb -s $s push "$repo\build\probe-fixture\Seen.txt"    /sdcard/Download/rlvm-probe/Seen.txt

# 2) 安装启动，然后在设备上点「选择游戏目录」并选 Downloads/rlvm-probe
& $adb -s $s install -r "$repo\app\build\outputs\apk\debug\app-debug.apk"
& $adb -s $s shell am start -n org.rlvm.android/.MainActivity

# 3) 读取结果（应用会自动跑一次）
& $adb -s $s logcat -d -s rlvm-native:V '*:S'
```

授权经 `takePersistableUriPermission` 持久化，之后点「运行 SAF 引擎」即可重复执行，无需再次选择。
「运行应用目录」按钮走普通路径，用于不依赖用户操作的自动化迭代。

## 6. 已知限制

- 引擎主循环尚未运行：`RLVMInstance::Run()` 仍硬编码 `SDLSystem`，需要 `AndroidSystem` 顶替（阶段 2）。
- 尚未验证 GLES 渲染与 AAudio 音频。
- `probeGameDir` 目前接受**普通文件路径**。正式的外部文件访问要走 SAF（T2.2）；应用外部目录只是为了让首轮验证不依赖 SAF。
