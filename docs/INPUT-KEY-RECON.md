# 方向键 / 运动输入 摸排（2026-10-06，未连真机，纯代码 + 反汇编）

> 目标：搞清楚「输入栏的方向键为什么进不了游戏（运动命令不成立）」。
> 本文只写**已验证的事实**与**下一步要做的实验**，不含猜测性改动。
> 相关：`dev-log/OPEN-ISSUES.jsonl`（2026-10-06 两条 issue）、
> `docs/HANDOFF-2026-10-06.md` §8。

## 1. 结论摘要（先说结果）

1. **平台侧链路是通的**：输入栏按钮 → `NativeBridge.keyEvent()` →
   `AndroidEventSystem::PostKeyEvent()` → `ExecuteEventSystem()` →
   `EventSystem::DispatchEvent()`；同一函数里 `nudgeCursor()` 还会发
   `touchEvent(TOUCH_MOVE, x, y, 1)`。签名、注册、排队都没问题。
2. **硬门槛在 `DispatchEvent`**（`rlvm-release-0.14/src/systems/base/event_system.cc:96`）：
   它只把事件交给 ①注册过的 `EventListener`（long operation 压栈时自己注册）
   ②**当前栈顶的 long operation**。也就是说 —— **脚本正在执行（栈顶不是 long op）时，
   按键事件被直接丢弃**。小游戏是"脚本自己循环 + 调 DLL"，不是 long op，
   所以那个阶段方向键走通用按键通路**必然无效**。
3. **脚本从不轮询方向键**：全库 `Sys150/151/152` 的参数里，键码集合只有
   `{0,1,2,3,4,5,49,65,66,80,81,...,89,100}` —— **没有 273~276（SDL 的下/上/右/左）**。
4. **`Sys 151/152` 的真实语义不是"鼠标二值"**：参数里那串数字是**要轮询的键码表**，
   151/152 的职责是「把这一组输入里当前被按下的那个**写进给定引用**」
   （`Sys151(intD[101], 5,4,49,0,1,2,3,100)`、`Sys152(intD[109], 81..89,80)`）。
   而我们的实现（`app/src/main/cpp/native-bridge.cpp` 的 `LbInputPollOp`）：
   * `Sys151` 只看**鼠标**：`value = (b1 == 1 || ClickSeenThisFrame()) ? 1 : 0`
     —— **完全忽略键码表**；
   * `Sys152` **恒写 0**（注释理由："全脚本只写不读"）。
5. **`intD[109]` 是死通道**（这条很关键，别再去修它）：
   * 脚本侧：全库只有 `Sys152` 写入，**没有任何读取点**；
   * DLL 侧：反汇编里**没有** `0x1b4(...)`（= intD[109]）访问。作为对照，
     已知被 DLL 读过的 `intD[1000]/[1004]/[1009]` 分别对应 `0xfa0/0xfb0/0xfc4`，
     在 `llvm-objdump` 输出里都能直接查到 —— 检索方法是有效的。
   ⇒ 「把 Sys152 写对」**不会**让移动生效。
6. 所以方向键在原生引擎里的作用只可能在**引擎侧**（不是脚本轮询）：
   RealLive 的箭头键是**虚拟光标移动 / UI 翻页**。
   * 翻页：`long_operations/pause_long_operation.cc` 的 `KeyStateChanged` 处理
     `RLKEY_UP/DOWN`（BackPage/ForwardPage），且 `graphics.is_interface_hidden()`
     时**第一次按只负责把 UI 显示回来**；
   * 光标：引擎自己把方向键翻译成鼠标移动（这正是 pad 里 `nudgeCursor()` 在模拟的东西）。

## 2. 三个待验证的分叉（任一环境一次就能分清）

| 实验 | 看什么 | 结论分支 |
| --- | --- | --- |
| A. 开输入栏，按方向键 | logcat 里有没有 `rlvm-input: key code=273 pressed=1`（**现成日志，不用改代码**） | 没有 → 问题在 Kotlin/pad（pad 默认是 GONE、没开；或触摸被 overlay 吃掉）；有 → 进 B |
| B. 同一次按 | 有没有 `[lb-ext] input poll mouse -> ...`；画面/光标是否变化 | 有 key 日志但画面无反应 → 与 §1.2 一致（当时栈顶不是 long op），需要按 §3.2 补引擎侧映射 |
| C. 只看光标 | `nudgeCursor()` 每 60ms 发一次 `touchEvent(MOVE,...)`，logcat 里会有 `rlvm-input: touch action=<MOVE> ...` | 光标在画面上动、游戏却没反应 → 说明游戏读的不是"我们的光标"，要按 §3.1 把轮询做成键码表语义 |

## 3. 建议的修法（按性价比排序，都还没做）

1. **让 `Sys151` 尊重键码表**（最小改动、最可能直接见效）。
   把 `LbInputPollOp` 从"鼠标二值"改成"按键表轮询"：
   * **必须保留**现有鼠标语义 —— `SEEN7420` 的挥棒判定是 `intD[101] == 1`，
     改坏它会让小游戏又卡住（这是之前花掉一整轮的回归点）；
   * 再把参数里的键码（`49/65/66/80..89/100` 等 ASCII/SDL 码）纳入判定：
     任一被按住 → 写入对应值。
   * **编码必须用 PC 真值标定**：在 PC 端同一场景按键，用
     `tools/pt00_probe.exe <pid> --full` 抓 `intD[101]`（以及任何变化的槽位），
     确认"哪个键 → 写什么值"，**不要凭直觉猜约定**。
2. **引擎侧补「方向键 → 光标移动」**（若要复刻原生手感）：在平台层把
   `RLKEY_UP/DOWN/LEFT/RIGHT` 直接映射成 `InjectMouseMovement()`，
   这样不依赖 long op。注意 pad 已经在做等价的 `nudgeCursor()`，
   先按 §2 的 A/C 确认哪条路有效，避免重复劳动。
3. **`Sys152` 按键码表实现**（低优先级）：当前没有消费者（§1.5），
   但实现正确能让 `intD[109]` 与原生一致，未来补丁/新场景用到就不会再踩。
4. **不要自创"脚本可读按键"接口**：RLVM 的 `src/modules/*` 里
   **没有任何** RLKEY/KeyState 读取 op；`EventSystem` 也只存 shift/ctrl 两个修饰键。
   原生引擎若真有脚本可读的按键通道，一定是我们还没实现的那批 Sys 号，
   应该按 §1.4 的"键码表"模型去补，而不是发明新语义。

## 4. 复验/取证清单（现成，不用改代码）

```powershell
adb logcat -d -s rlvm-input:V '*:S'      # 按键/触摸到没到 native（§2 A/C）
adb logcat -d -s rlvm-stdout:V '*:S' | Select-String 'lb-ext|input poll'
adb logcat -d -s rlvm-stderr:V '*:S'     # pt00 侧（小游戏进出、WATCH 环等）
```

静态检索（本机，验证过有效）：

```powershell
# DLL 是否读取某个 intD 槽：intD[i] 的偏移 = i*4
llvm-objdump -d '<PT00.dll>' | Select-String '0xfa0\(|0x1b4\('

# 脚本里用过哪些 Sys op / 轮询了哪些键码
rg -o 'op<1:004:\d{5}, \d>' build/rlvm-scenes.txt | Sort-Object -Unique
```

## 5. 本次摸排改了什么

**没有改任何运行代码** —— 纯静态摸排（事件系统 + 模块实现 + 脚本反汇编 + DLL 反汇编）。
本文与 `dev-log/INPUT-KEY-RECON.jsonl` 是全部产物。
