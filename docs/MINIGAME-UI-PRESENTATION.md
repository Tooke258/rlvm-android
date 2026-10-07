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
