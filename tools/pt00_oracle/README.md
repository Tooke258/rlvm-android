# PT00 oracle（PC 侧原生对照）

把原版 `PT00.dll` 直接跑起来，喂一串调用、记录每步对 intD 的改动——用作
「真值轨迹」，与我们的实现（或执行器）逐项对比。设计依据见
`docs/LB-MINIGAME-RETRO.md` §4。

## 为什么这么小

`PT00.dll` 只导入 KERNEL32，和引擎的全部交互就两件事：

```
reallive_dll_func_load(ctx, a2)        // ctx 偏移 +0x14 处放 intD 基址
reallive_dll_func_call(func,a1..a4)    // 数据全走 intD
```

两条链路都已在 PC 上实测验证（`obj[0]=vtable`、`obj[1]=intD 基址`、
`obj[3]=基址+0x348=&intD[210]`、`obj[4]=基址+0x3E8=&intD[250]` 三处自检全中）。

## 构建

```powershell
tools\pt00_oracle\build.bat          # 32 位；脚本里按需改 vcvarsall 路径
```

## 运行

```powershell
tools\pt00_oracle\oracle.exe <PT00.dll> <calls.txt> [intd_snapshot.bin]
```

`calls.txt` 每行一次调用，`func a1 a2 a3 a4`（十进制或 `0x` 十六进制，后四个可省）：

```
# 示意
30
31
10 0 0 0 0
```

输出：每次调用后一行，含调用序号 / func / 参数 / 返回值 / **intD 里发生变化的槽位**；
带第三个参数时额外把 `intD[0..1999]` + `intF[0..1999]` 整片写成二进制，供逐帧 diff。

## 它解决的两个问题

1. **对照**：不再靠手抄 PC 内存里的若干个数，而是拿到"同一起点上，原版每步把 intD 改成了什么"。
2. **覆盖测量**：我们的执行器缺哪条指令，跑一遍就知道（解释器遇到未实现指令时打日志）。
