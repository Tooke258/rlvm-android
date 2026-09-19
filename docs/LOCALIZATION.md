# 汉化（Kud Wafter 中文补丁）接入方案

> 记录时间：2026-09-19
> 涉及本地文件（**均不进仓库**）：`local-data/dump-translation.tsv`、
> `C:\Users\tooke\Desktop\REALLIVE_chs.v0.26.DMP`

## 1. 现状：译文在哪

| 位置 | 内容 | 结论 |
| --- | --- | --- |
| 游戏目录 `SEEN.TXT`（3.5MB） | **日文原版**（日志里渲染出 `が`/`ド`/`キ` 可证） | 压缩容器，我们引擎能读 |
| `REALLIVE_chs.v0.26.exe`（2.2MB） | 只有 14 条中文串，全是 Config 工具界面文字 | **不含剧本** |
| `REALLIVE_chs.v0.26.DMP`（300.9MB，进程内存转储） | 2398 条 GBK 中文串（含剧本正文） | **译文唯一来源** |

补丁是**运行时加载**译文的加载器：内存里有解压后的场景数据 + 自己的场景编号索引
（升序整数数组，含跳号），磁盘上没有对应的中文文件。

## 2. 已经排除的方案

**整块导出 SEEN 容器**：内存里没有与 `SEEN.TXT` 同构的容器。判据——日文 `SEEN.TXT`
里不存在内存中那串场景编号数组（搜 4 字节序列 `2718,2719,2720,2721` 在磁盘文件里
命中 0 次，在转储里命中 8 次且彼此等距 15632 字节）。所以内存布局是加载器私有的，
逆向它成本不可控。

## 3. 采用的方案：运行时译文对照表

不重建容器、不写打包器，改为在渲染文字前查表替换：

1. **导出日文侧文本**：引擎加一个诊断开关，把 `SEEN.TXT` 每个场景的文本串按顺序
   导出（`场景号 / 序号 / 日文`）。属于只读诊断，不动语义。
2. **对齐**：与已提取的 2398 条中文串按场景与顺序配对，得到 `日文 → 中文` 对照表。
   用"每场景句子数是否一致"做自检，对不上的场景单独标记。
3. **渲染期替换**：文字进渲染层前查表，命中则渲染中文。

优点：不需要实现 RealLive 场景打包器（重压缩 + 目录表重写），验证轮次少；
对照表是纯文本，本地存一份就是全部译文，日/中可随时对照。
已知代价：中文与日文的字宽/换行可能需要微调（在渲染层调，不涉及数据）。

## 4. 本地留档

| 文件 | 说明 |
| --- | --- |
| `C:\Users\tooke\Desktop\REALLIVE_chs.v0.26.DMP` | 内存转储副本（`Temp` 会被清理，这份是保险） |
| `local-data/dump-translation.tsv` | 2398 条 `偏移<TAB>中文`，UTF-8 |
| `build/dump-translation.tsv` | 同一份工作副本 |

提取方法（可复现）：把转储按 8MB 分块，用 Latin-1 逐块转字符串后正则找
`(?:[\x81-\xFE][\x40-\x7E\x80-\xFE]){6,}` 的 GBK 双字节长串，再用「必须含高频汉字」
过滤掉随机二进制噪声。注意：单靠字面过滤不能归零，仍有少量误命中（如随机序列凑出
含"不"字的串），后续对齐时用场景边界校验来剔除。

## 5. 状态

- [x] 确认译文位置与形态
- [x] 排除整块容器导出
- [x] 提取并本地保存译文串
- [ ] 引擎侧日文文本导出开关
- [ ] 日/中对照表生成与自检
- [ ] 渲染期替换 + 真机验证

## 6. 导出开关的实现要点（已确认的接口）

下一步要实现 `export_jp_text=1` 的实际导出，相关上游接口已经查清：

```cpp
// libreallive/archive.h
Archive::begin() / end()          // 遍历 std::map<int, FilePos>（场景号 -> 位置）
Scenario* Archive::GetScenario(int index);

// libreallive/scenario.h
class Scenario {
  int scene_number() const;                     // 场景号
  const_iterator begin()/end();                 // 遍历字节码元素
};

// libreallive/bytecode.h
class TextoutElement : public BytecodeElement {
  const string GetText() const;                 // 文本（CP932 原始字节）
};
```

导出流程：`for (场景 in archive)` → `GetScenario(场景号)` → 遍历 `begin()/end()` →
`dynamic_cast<const TextoutElement*>(...)` → `GetText()` → `cp932toUTF8(..., 0)` →
按 `场景号 / 序号 / 文本` 写 TSV。

**待确认的唯一细节**：`Script::BytecodeList` 的元素是指针还是引用（决定用
`dynamic_cast<const TextoutElement*>(e->get())` 还是 `&*e`）；`scenario.h` 里
`typedef BytecodeList::const_iterator const_iterator` 说明它定义在 `Script` 内部，
下次先看一眼 `Script` 的成员声明即可，无需再猜。

导出产物走应用外部文件目录（`/sdcard/Android/data/org.rlvm.android/files/jp-text.tsv`），
用 `adb pull` 取回。自检：序章那句「心臓がドキドキ…」应出现在导出结果里
（验证遍历顺序正确，这是后续与中文串对齐的前提）。
