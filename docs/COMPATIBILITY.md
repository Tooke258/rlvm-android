# 汉化补丁与兼容性

> 记录于 2026-09-19。回答「非原版、带汉化补丁的游戏怎么处理」。

## 1. 选择的对象是游戏根目录，不是 EXE

RLVM 是 RealLive 虚拟机的**重实现**，不加载也不执行 `RealLive.exe`——代码中从未引用过这个名字。
启动时它只在给定目录里找两个文件：

```cpp
FindGameFile(gamerootPath, "Gameexe.ini");   // 配置
FindGameFile(gamerootPath, "Seen.txt");      // 字节码
```

目录里唯一与 `.exe` 相关的是**反向检查**：`avg3216m.exe` / `avg3217m.exe` / `siglus*.exe`
被发现就报错拒绝，因为那是别的引擎。

这也是 Android 侧用 `ACTION_OPEN_DOCUMENT_TREE`（选目录树）而不是 `ACTION_OPEN_DOCUMENT`（选文件）的原因。

## 2. 补丁是怎么工作的

汉化补丁本质是**在同一目录里替换或新增数据文件**，因此「选根目录」这个模型天然兼容补丁。

| 补丁动作 | RLVM 的处理 |
| --- | --- |
| 替换 `SEEN.TXT` | 直接生效，最常见的形式 |
| 追加 `SEEN####.TXT` 覆盖文件 | `Archive::ReadOverrides()`（路径后端）/ `Archive::ApplyOverrides()`（SAF 后端）扫描目录并覆盖对应场景索引 |
| 修改 `Gameexe.ini` | 直接生效 |
| 附带 `rlBabel.dll` | RLVM **在进程内重新实现**了该 DLL（`RlBabelDLL : public RealLiveDLL`），接管 `TextoutAdd` / `TextoutLineBreak` / `TextoutGetChar` 等调用 |
| 附带字体 | 通过 `#__GAMEFONT` 或命令行 `--font` 指定，`FindFontFile` 会优先采用 |

rlBabel 是关键：RealLive 允许游戏调用外部 DLL，补丁借此改变换行与编码处理。
`RealLiveDLL::BuildDLLNamed()` 中 `if (name == "rlBabel")` 直接返回 RLVM 自己的实现，
所以补丁以为在调 DLL，实际被 RLVM 接住。

## 3. 编码与字体

- 补丁的 `SEEN.TXT` 自带编码标记，`Archive::GetProbableEncodingType()` 读出，
  `RLMachine::GetTextEncoding()` 返回当前场景的 `scenario->encoding()`。
- 西文补丁（编码 2）会触发 `system().set_use_western_font()`，`FindFontFile` 据此更换字体候选。
- **中文补丁**通常用 CP936，而 RLVM 的 `encodings/cp936.cc`（2825 行码表）已经支持转换。
  缺的是中文字体与字形渲染——那要等 T4.2/T4.3 接入 FreeType。
- 注意 `msgothic.ttc` 是商业字体，**不能随应用分发**。需要提供用户自选字体的入口。

## 4. 存档兼容

`RLVMInstance::DoUserNameCheck()` 处理「先跑日文原版、再装补丁」导致 global memory 中名字乱码的情况，
会弹窗让用户选择重置还是带着坏名字继续。Android 侧需要把这个对话框接起来。

## 5. 当前 Android 端的支持边界

| 能力 | 状态 |
| --- | --- |
| 选根目录（SAF 目录树） | 已实现并持久化授权 |
| `SEEN.TXT` + `Gameexe.ini` 经 SAF 读取 | 已实现（fd + mmap / 整体读入） |
| `SEEN####.TXT` 场景覆盖 | **已实现并验证**（路径后端与 SAF 后端结果一致） |
| rlBabel 补丁 | 代码在编译范围内；尚未用真实补丁实测 |
| 游戏资源查找层（`#FOLDNAME` + 目录枚举 + 扩展名匹配） | **已实现并验证**：抽象为 `GameFileSystem`，普通路径后端保持上游行为，SAF 后端以相对路径为不透明标识 |
| 图像经 SAF | **已实现并验证**：`FindFile` → `OpenFd` → `GRPCONV` 解码（PDT / G00 / BMP）→ Surface → 合成显示 |
| 音频经 SAF | 查找层已就绪；**加载器**待 T4.1 实现（AAudio 后端） |
| GRP type-2 多子图 | **未实现**：区域表尚未接入，`Surface::GetPattern` 用基类默认值 |
| PNG / JPEG 解码 | 未启用（解码器存在，但需要 libpng / libjpeg；RealLive 原生格式用不到） |
| 字体注入（`#__GAMEFONT` / 自选字体） | 未实现，依赖 FreeType 接入 |
| 存档读写经 SAF | 未实现 |
| 大小写不敏感的文件名解析 | **已实现并验证**：路径后端用上游 `CorrectPathCase()`，SAF 后端在 `SafFileSystem` 内逐级解析（精确匹配优先，失败后列目录做 ignoreCase 比对，结果带缓存） |

## 6. 明确不支持的情况

只有 RLVM 已重新实现的 DLL 能被接管。目前实现的有 `rlBabel`，以及数个游戏专用 DLL
（Little Busters 的 `ef00`/`pt00`、Tomoyo After 的 `dt00`）。
补丁若依赖其它自研 Windows DLL，在任何平台都无法运行——这与 Android 无关。

## 7. Android 平台差异（实现时实际踩到的）

| 差异 | 表现 | 对策 |
| --- | --- | --- |
| Adreno 驱动对 GLSL `#version` 的位置极其严格 | 规范允许 `#version` 前有空白，但高通驱动报 `P0005: #version must be on the first line`，着色器编译失败且**链接时报的错是误导性的** | 着色器源码用 `trimIndent()` 去掉起始换行；并且**必须检查编译/链接状态并打印 info log**，否则失败是静默的 |
| `fs::file_size` 对目录的行为 | Linux 上对目录取 size 允许，Android 抛 `Function not implemented` | 只对普通文件取大小；判断是否为目录要用 `is_directory` |
| 文件名大小写 | 见第 5 节：SAF 的 documentId 精确匹配，而游戏来自 Windows/FAT | 路径后端用上游 `CorrectPathCase()`，SAF 后端在 `SafFileSystem` 内逐级解析 |
| SAF 的 `ACTION_OPEN_DOCUMENT_TREE` 授权 | 必须由用户在系统选择器中完成一次，无法用 adb 或代码绕过 | 授权后用 `takePersistableUriPermission` 持久化，后续无需重复操作 |
| PowerShell 5.1 读取无 BOM 的 `.ps1` | 按 ANSI/GBK 解码，中文字符被误解码后可吞掉后续行 | 仓库内的 `.ps1` 一律保持纯 ASCII（脚本头部已注明） |
