# Sys 1005 与「棒球小游戏的球永远不飞」

> 2026-10-07。本文只讲一条链：**为什么手机上球不出现，而 PC 上正常**。
> 前置阅读：`docs/HANDOFF-2026-10-07.md`、`docs/PT00-CALLSITES.md`。

## 0. 一句话结论

上游 RLVM 把 `Sys 1005` 实现成了 `int((v1-v3)/(v2-v4))`，而 LBEX 的棒球脚本
把这支函数当**两点距离**用（`intL[0] < 35` 决定「球是否已经到达打者」）。
上游公式在真实数据上恒得 0 → 阈值判断恒成立 → 脚本的「球复位」块每帧执行
→ `intD[220]`（球的可见位）永远为 0 → **球永远不出现**。

修复：`Sys 1005` 改为两点距离。改动落在
`rlvm-release-0.14/src/modules/module_sys.cc`（文件开头有同样的反证注释）。

## 1. 证据链

### 1.1 球的显隐由 `intD[220]` 决定（脚本侧）

`build/rlvm-scenes.txt` → `SEEN7340`：

```
#line 97   if (intD[210] == 1 || intD[210] == 2)
#line 100  objShow(203, 0, intD[220])     ← 球对象 203 的显隐
#line 101  objShow(202, 2, intD[220])
#line 102  if (intD[220] == 1) { … 投影/移动 … }
#line 103  CallDLL(0, 10, intD[226], intD[227], intD[228], intD[245])
```

### 1.2 槽位布局（DLL 侧）

由 `pt00_launch_ball`(0x10006820) 的直写序列反推（`*(intD + 880)` = `intD[220]`）：

| 槽 | 含义 |
| --- | --- |
| `intD[210]` | 投球状态机（0 不在投球 / 1 飞行 / 2 守备） |
| `intD[220]` | 球 1 **可见位**；`[221]` 图案号；`[222]` 标志 |
| `intD[223..225]` | 出手点 |
| `intD[226..228]` | 球当前位置 |
| `intD[229..231]` | 上一帧位置 |
| `intD[250..]` | 球 2（镜像；x/z 翻倍或折半） |

`pt00_mirror_ball_to_screen`(0x100031B0) 把 `[220..231] → [250..261]`，
所以 `intD[250] = intD[220]`（可见位同步）。

### 1.3 真机现状（`build/lc_ball.txt`，10-07 00:40 那一轮）

```
frame 3300 intD[210]=1 … stm 600=4 630=207 …
ent… | ball1 intD[210..232]=1,0,0,0,0,0,0,0,0,0,0,255,0,-39,1,4086,-39,207,4086,-39,206,4086,0,
      | ball2 intD[250..262]=0,255,0,0,0,0,-78,207,4086,-78,206,4086,0,
```

⇒ `intD[210]=1`（在投球中）但 **`intD[220]=0`、`intD[250]=0`**（球被隐藏），
且 `[222]=0`、`[232]=0`（球速为 0）、`[600]=4`、`[630]` 一直在涨（投球动画反复重启）。

### 1.4 这段值正好是脚本「球复位块」的输出

`SEEN7420`:

```
#line 267  intL[0] = Sys1005(intD[610], intD[612], intD[226], intD[228])
#line 268  if (intD[625] == 0 && intD[210] == 1 && intL[0] < 35)
#line 270      intD[630] = 0
#line 271      intD[600] = 4
#line 273      intD[220] = 0        ← 隐藏球
#line 274      intD[221] = 255
#line 275      intD[222] = 0
#line 276..285 intD[223..231] = 复制 intD[226..228]
#line 286      intD[232] = 0        ← 球速归零
```

手机上 `[625]=0`、`[210]=1` 都成立，于是**唯一能解释「每帧复位」的就是 `intL[0] < 35` 恒成立**。

### 1.5 上游实现被 PC 实测值直接反证

上游（`module_sys.cc`，改名前后都是这支）：

```cpp
struct Sys_modulus : public RLOp_Store_4<IntConstant_T, IntConstant_T,
                                         IntConstant_T, IntConstant_T> {
  int operator()(...) { return int(float(var1 - var3) / float(var2 - var4)); }
};
```

PC 原生引擎的实测值（`build/pc-intd-full.txt` 的 `[610]/[612]` + `output.txt` 的飞行帧）：

| | 值 | 来源 |
| --- | --- | --- |
| `intD[610]` | `-140` | PC 全量 dump |
| `intD[612]` | `2100` | PC 全量 dump |
| `intD[226]` | `-55` | PC 飞行帧 |
| `intD[228]` | `1471` | PC 飞行帧 |
| `intD[220]` | **`1`（球可见、在飞）** | PC 飞行帧 |

上游公式 → `(-140 + 55) / (2100 - 1471) = -85 / 629` → 整数截断 → **`0`**。
`0 < 35` 成立 ⇒ 复位块应当每帧执行 ⇒ 球应当被隐藏。
**但 PC 上球是可见的** ⇒ 上游公式必错。

两点距离 → `sqrt(85² + 629²) = 634` ≥ 35 ⇒ 不复位 ⇒ 与 PC 一致 ✔

### 1.6 影响面

`Sys 1005` 在 LBEX 全库只有 **1 处**调用（`rg -o 'op<1:004:01005, 0>'` = 1），
就在上面这段棒球逻辑里。所以这条语义修正只影响这一个调用点。

同一份 `Sys` 表里 `1006 "angle"` 的函数体与 `1005` **逐字相同**，也是占位猜测；
目前 LBEX 没有用到 1006，先不动。

## 2. 改动

| 文件 | 改动 |
| --- | --- |
| `rlvm-release-0.14/src/modules/module_sys.cc` | `Sys_modulus` → `Sys_pair_distance`（两点距离）；注册名 `"modulus"` → `"dist"`；新增 `g_sys1005_legacy` / `g_sys1005_trace`；文件开头写反证 |
| `app/src/main/cpp/native-bridge.cpp` | diag 键 `sys1005=legacy\|fixed`、`sys1005_trace=N`；`extern` 声明放在匿名 namespace **之外** |
| `tools/rlvm-diag.sys1005-trace.txt` | 取证用 diag 样例（`sys1005_trace=40` + `lb_minigame=1`） |

## 3. 真机验证怎么做

1. 推 diag：`adb push tools/rlvm-diag.sys1005-trace.txt /sdcard/Android/data/org.rlvm.android/files/rlvm-diag.txt`
   （**diag 只在引擎启动时读一次**，改完要 `am force-stop` 再开）。
2. 走到棒球小游戏，看两件事：
   * 球是否从远处（屏幕上）飞向打者（屏幕下），约 2 秒一球；
   * `logcat -s rlvm-stderr` 里 `[sys1005] (…) -> dist=… legacy=…`：
     `dist` 应在球接近打者前一直 ≥ 35，接近后掉到 < 35；`legacy` 应恒为 0。
3. A/B 反证（可选）：把 diag 改成 `sys1005=legacy`（去掉 `sys1005_trace`），
   球应当**重新变成不出现**。这一步能把根因钉死到「就这一支函数」。

## 4. 还没解决的（不要混在一起）

* 暂停菜单「先正常、随后全变棗铃」＝呈现层问题（数据侧已证明正确）。
* 小游戏实体渲染丢失 / 猫不出现；打者渲染。
* 卡顿（`loaded "…"` 前平均 96.5 ms）。
* `intD[1800..1804]` 是"茶壶/茶杯"槽，**不是**棒球的球（早期误判过一次）。
