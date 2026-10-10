# v0.3.1

修一个 v0.3.0 引入的**音频回归**，外加一个输入面板的整理。版本号只跳补丁位：
功能面与 v0.3.0 相同（小游戏首次接入 + 存档/相册/选择枝），但**建议所有 v0.3.0 用户升级**。

## 修复：BGM 每次换曲只播两秒就被截断

**症状**：游戏里每次换曲（场景切换、事件音乐），新 BGM 播约 2 秒后被硬切掉。

**根因**：v0.3.0 为修「相册 Scene 回想点进去必卡死」把 `BgmFadeOut` 改成了
`AudioEngine::FadeOutAndStop`（音量线性淡出 + **到点真正停通道**；回调用
`stop_at_ms` 判定，且特意挪到"读环形缓冲之前"，否则缓冲排空后停播请求永远不会被处理）。
但 `AudioEngine::Play()` 起新曲时只清了 `fade_out_request` / 淡入淡出参数，
**没有清 `stop_at_ms`**：

```
bgmFadeOut(2000)  → stop_at_ms = now + 2000
bgmPlay(新曲)     → 新曲起在同一通道上，那条停播待办还挂着
        …2 秒后…  → 回调发现到点 ⇒ 把新曲停掉
```

**修法**：在"新音源装上"的三个点都清掉它 —— `Play()` 的 pending 分支、`Play()` 的
冷启动分支、以及解码线程把 `pending_source` 接手的那一刻（最权威的一处）。
`bgmFadeOutEx` 的"真淡出并停"语义保持不变，所以相册那条修复不会回退。

## 变更：输入面板去掉「加速」按钮

原「加速」按钮发的是键盘 `SHIFT`（当初是给脚本侧预留的通道，没有消费者），
与「Ctrl」（按住 = 引擎 `ShouldFastForward` 那条真正的强制快进）功能重复，故移除。
需要快进时用「Ctrl」。

## 升级注意：全局内存（收集档 / 设置）不跨这一代布局

v0.3.0 起 `SIZE_OF_MEM_BANK` 由 2000 扩到 8192（D-043，修 LBEX 相册页码表读不到），
**这些存储区会被序列化**，因此：

* **v0.2.4 及更早的 `global.sav.gz` 在 v0.3.0+ 上加载失败**（实测日志：
  `global memory not loaded (first run?): ... rename: Invalid argument`），
  之后引擎会按新布局重新生成一份（设置/收集进度需要重来或重新解锁）；
* 游戏内的原生存档（`SAVEDATA/*.sav`）**不受影响**。

需要"全解锁收集档"时，用 `tools/rlvm-diag.perfect.txt`（`kidoku_unlock_all` +
`cgm_unlock_all` + `collection_unlock_all`，由引擎自己写、不要手改归档）跑一次即可。

## 其余与 v0.3.0 相同

已知遗留、真机验证范围、`rlvm-diag.txt` 常规配置（`lb_minigame=1` + `logic_hz=37`，
**删掉会让小游戏被跳过 / 节拍回退**）等见 v0.3.0 的说明与 `README.md`。

安装包：`app-release.apk`（arm64-v8a / armeabi-v7a / x86_64，minSdk 31）。
