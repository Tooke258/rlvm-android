# 小游戏 UI 呈现层：菜单图标「全变默认图像」+ 渲染残留

> 状态：**取证中**。本文只记录已经**机械可验证**的事实与工具，结论待一次真机渲染树对齐。
> 相关：`docs/HANDOFF-2026-10-07.md` §1.4（"数据侧正确 ⇒ 剩呈现层"那句结论的前提已变，见 §4）、
> `docs/MINIGAME-HIT-DIRECTION.md`（同一套"先取证再改"的路子）。

## 1. 症状（用户口径）

1. **UI 全部照默认图像渲染**：菜单/按钮类 UI 元素各自渲染成"0 号图案"，看起来全一样。
2. **UI 渲染残留**：状态已经过去了（菜单关了 / 高亮移走了），旧图像还留在原地。

## 2. 已核实的事实

### 2.1 按钮素材的真实结构（把图**导出成 PNG 看了一眼**）

`PT_RMENU_BTN00.g00` 解码后是 **205×123 = 5 列 × 3 行 = 15 个子图**，目视确认：

| 列 | 状态 |
| --- | --- |
| 0 | 普通 |
| 1 | 悬停（更亮） |
| 2 | 按下（最亮） |
| 3 | 失效（灰 + 大叉） |
| 4 | 高亮/描边 |

行 = 图标：`タイトル` / `練習終了` / `練習再開`。**即"每个图标占一行 5 格状态"。**

工具（本轮新增）：`tools/g00_decode_probe.exe --raw <prefix> <a.g00>` 导出解码结果，
再用 `python tools/rgba_to_png.py <in.rgba> <out.png> <w> <h> [scale]` 转 PNG。

### 2.2 脚本的图案号写法就是"第 k 行第 0 格"

`SEEN7800` index3（每帧 `farcall(7800,0)`）对 221 号的子对象写：

```
op<2:081:01039>(221, 1+intL[0], k * 5)      # 01039 = objPattNo
```

`k*5` 恰好是"行 k、列 0"（列数 = 5）。同一套写法也用在 `PT_CAM_BTN00` 上，而那个文件是
**205×615 = 75 个子图（5 列 × 15 行）**——完全装得下 `k=0..14`。
⇒ **`k*5` 是直接下标写法，不是"5 倍暗号"。**

（扫描工具：`python tools/script_pattno_scan.py build/rlvm-scenes.txt`；全库 `objPattNo` 绝大多数是
小常数 0/1/2/3…，只有 `PT_CAM_BTN00` / `PT_RMENU_*` 这几处成 5 倍。）

### 2.3 越界图案号的下场：**退回 0 号**（RLVM 系都是这样）

```cpp
// rlvm-release-0.14/src/systems/base/surface.cc 与 sdl/sdl_surface.cc
const Surface::GrpRect& SDLSurface::GetPattern(int patt_no) const {
  if (patt_no < region_table_.size()) return region_table_[patt_no];
  else                                return region_table_[0];   // ← 越界 → 0 号
}
```

我们的 `AndroidSurface::GetPattern` 同样实现。**所以任何越界图案号都会静默变成"0 号图案"**，
这正是"全部照默认图像渲染"这个观感的**唯一已知机制**。

### 2.4 上一份渲染树 dump 是**修复前**的，不能用它下结论

`build/applog-menu2.txt` / `build/applog-patno.txt` 里 221 的所有子对象都是 `patt=0` +
`Rendering Rect(0,0,41,41)`；但那份 dump 抓于 **`goto_case` 修复之前**（当时 switch 落到第一个分支，
所以所有按钮都是 `0*5`）。修复后的 `[patno]` 日志显示写入值已经是 0/5/10/30/35/40/50/55 且读回一致。

⇒ **暂停菜单的两个 `PT_RMENU_BTN00` 按钮（patt=5、10）在修复后应当画的是"練習終了/練習再開 的普通态"**，
也就是说图标这一半**可能已经好了**；必须用一次**当前版本**的渲染树来确认（见 §5）。

## 3. 待验证的假设（别当结论用）

* 残留（"已经过去了但 UI 还在原地"）**不在每帧清屏那一侧**：`AndroidGraphicsSystem::BeginFrame()`
  每帧都对帧缓冲 `Fill(黑色不透明)`，`EndFrame` 后整帧交给 GL 上屏 ⇒ 帧缓冲不会自己留残影。
  所以残留更可能来自**引擎侧的对象生命周期**（`ClearAndPromoteObjects` 的晋升/擦除、
  `objFgWipeCopy` 保护）或**某个 blit 被跳过**。
* 有一个候选机制值得查：`AndroidSurface::BlitToSurface` 用**源面的内容包围盒**裁剪，
  与内容不相交时**整块跳过**（`android_graphics.cpp` 的 `g_blit_fast_enabled` 分支）。
  若某源面的 `content_*` 是「过期且偏小」的保守盒，blit 会被跳过 ⇒ 目标面保留旧内容 ⇒ 正是残留形状。
  已知 `Clone()` **不复制内容包围盒**（`pixels_` 复制了、`content_*` 没有）——目前判断是"偏慢但不算错",
  但在"克隆出来的面被当作源"的路径（wipe-copy 存底）上会把包围盒优化整体失效。**未验证。**

## 4. 对 `HANDOFF-2026-10-07.md` §1.4 的更正

原文：「数据侧已证明完全正确 ⇒ 剩下的是呈现层」。**前半句的前提已变**：那份"数据正确"的取证里，
`patt` 的写入值确实对，但**越界退回 0 号**这条规则当时没被纳入判断（0 号恰好是个合法图案，
所以"有图案、且写入正确"与"画的是 0 号"可以同时成立）。§2.3 已把这条机制补上。

## 5. 下一步（需要一次真机取证）

```powershell
adb push tools/rlvm-diag.ui-tree.txt /sdcard/Android/data/org.rlvm.android/files/rlvm-diag.txt
adb shell am force-stop org.rlvm.android        # 重开 App，进小游戏、呼出暂停菜单
# 面板 →「导出渲染树」，然后：
adb pull /sdcard/Android/data/org.rlvm.android/files/rlvm-log.txt build/applog-ui-tree.txt
```

判据：树里 `PT_RMENU_BTN00` 的每个子对象看两行——
`Object #N … patt=?` 与紧跟的 `Rendering Rect(src) to Rect(dst)`。

* `patt=5/10` 且 `src=(41,0)-(81,40)` / `(82,0)-(122,40)` ⇒ 图标侧已正确，问题只剩残留；
* `patt=0` 或 `src=(0,0)-(40,40)` ⇒ 图案号仍被写成 0 / 被覆盖 / 越界 ⇒ 继续查写入侧；
* 关掉菜单再打一次 dump：若树里已经没有这些对象、但屏幕上还在 ⇒ 残留确认在执行侧（合成/擦除）。

## 6. 2026-10-07 深夜 · 渲染树对完了：**图案号是对的，是合帧时机错了**

### 6.1 真机渲染树 + 累积日志给出的硬事实

用户按 §5 抓了一次**当前版本**的渲染树（`build/applog-ui-tree.txt`，7.8 万行，
应用日志是跨会话累积的，里面还留着上一轮 `patno_trace` 的记录）。三条硬事实：

1. **图案号写得完全正确**。同一帧里：

   ```
   [patno] SEEN7800 L131 parent=221 child=8  set=30 now=30
   [patno] SEEN7800 L131 parent=221 child=9  set=35 now=35
   [patno] SEEN7800 L131 parent=221 child=10 set=40 now=40
   [patno] SEEN7800 L131 parent=221 child=12 set=50 now=50
   [patno] SEEN7800 L131 parent=221 child=14 set=55 now=55
   [patno] SEEN7800 L162 parent=221 child=15 set=0  now=0     ← 暂停菜单按钮 1
   [patno] SEEN7800 L172 parent=221 child=16 set=5  now=5     ← 暂停菜单按钮 2
   [src]   PT_CAM_BTN00 patt=0 / 30 / 35 / 40 / 50 / 55 …     ← SrcRect 真的用上了这些值
   ```

   ⇒ §2.3 那条"越界退回 0 号"**没有发生**（0/5/10 都在 15 个子图范围内），
   图标映射本身是好的。**上一轮"越界猜测"到此作废。**

2. **每帧都有一轮"重置"**：`[params-reset] InitializeParams after=SEEN7800 L121 objChildFgInit`
   在日志里出现 17 次 —— 菜单每帧先把子对象**重置回默认参数（patt=0）**，同一帧再重建。
   这是引擎的正常写法（每帧重建 UI），问题不在这条指令本身。

3. **但我们抽到的渲染树里 `patt=0`**：`Object #16 … patt=0 … Rendering Rect(0,0,41,41)`。
   而 `导出渲染树` 走的是 `System::DumpRenderTree() → graphics().Refresh()` ——
   **一次独立合成**。它拍到 0，说明**存在"对象已重置、还没重建"的中间态会被合成上屏**。

### 6.2 根因：合帧时机与上游相反

```cpp
// app/src/main/cpp/android/android_system.cpp（修改前）
graphics_->ExecuteGraphicsSystem(machine);
if (platform()) platform()->Run(machine);      // ← 先跑脚本（重置 → 重建）
...
graphics_->Refresh(nullptr);                   // ← 再合成

// 上游 SDL（sdl_system.cc + SDLGraphicsSystem::ExecuteGraphicsSystem）
//   if (is_responsible_for_update() && screen_needs_refresh()) { Refresh(); OnScreenRefreshed(); }
//   —— 合成发生在**跑脚本之前**，所以合成到的永远是"上一遍完整搭好"的状态。
```

脚本一旦在「`objChildFgInit` 重置 → `objOfFile`+`objPattNo` 重建」之间让出
（`wait` / 长操作 / 分帧构建），**脚本之后合成**就会把刚被重置成默认的对象合成上屏。
这与用户的描述逐字吻合：**"元素已经被正确排列好了，完全渲染的下一刻全部切回了默认"**，
而且**与场景无关**（相册同理）。

### 6.3 修法（先做成可 A/B 的开关）

新增 diag `refresh_before_run=1`（`tools/rlvm-diag.refresh-before.txt`）：
打开时按上游次序**在跑脚本之前**合成；默认仍为原行为，便于对比。
`android_system.cpp` 里把合成那段抽成 `RefreshAndClearDirty()`，两处按标志二选一调用。
引擎启动报告的 diagnostics 行会打印 `refresh_before_run=on/off` 以便确认生效。

## 7. 2026-10-07 深夜（续）· **时序假设作废**；差异锁定为「页码图案号变 0」

### 7.1 `refresh_before_run=1` 真机 A/B：**没修好**

用户打开 `refresh_before_run=1` 后仍然"刷回默认" ⇒ §6.2 那条"合帧时机"假设
**不是根因**（最多是放大器）。开关保留（默认关），不再作为结论。

### 7.2 相册两份渲染树逐项对照：差异**只有 `patt`**

用户导出了相册的两个状态（正确 / 默认），逐对象对照结果：

| | 正确那份 | 默认那份 |
| --- | --- | --- |
| `O_CGM_NUM00` 各子对象 `patt` | **7 / 6 / 3 / 4 / 5 / 7 / 5 / 4 …** | **全部 0** |
| `xy` / `Image:` / `vis` / `data` | 完全一致 | 完全一致 |

⇒ 位置、素材、可见性全都对，**只有图案号被写成/读成 0**。

### 7.3 相册页码的真实生成点（脚本静态）

`SEEN9515`（CG 相册）每帧重建这些子对象：

```reallive
#line 301   if (intL[20] >= 1 && intL[21] >= 2)
#line 303   objOfChild(layer 209, i, "O_CGM_NUM00", ...)
#line 304   objPattNo (layer 209, i, intL[20] + 1)     ← 正确那份的 7/6/3/4/5 出自这里
#line 307   objPattNo (layer 210, i, intL[21] + 1)
#line 310   objPattNo (layer 211, i, 0)               ← 这一层**本来就写 0**
```

`intL[20]`/`intL[21]` 来自更上游的页码表（`intL[21] = intA[900 * 8 + intL[11]]`，L278）。
**`patt=0` 本身就是这个素材的合法值**（211 层恒用 0），所以"默认图案"= 页码位画成 0 号图。

待查（下一步取证）：

1. L304/L307 的 `set=` 到底写了几（0 还是 6/7）—— 即页码值本身错，还是写入后被重置；
2. `[params-reset] InitializeParams after=…` 是否出现在那两条写入之后。

取证配置：`tools/rlvm-diag.gallery-patno.txt`（`patno_trace=1`）。
复现：启动 → 进相册 → 停在页码不对的那一屏约 5 秒 → 退出 → 拉 `rlvm-log.txt`。
