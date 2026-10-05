// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2007 Elliot Glaysher
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
//
// -----------------------------------------------------------------------

#include "modules/module_mov.h"

#include <string>
#include <chrono>

// 平台相关实现（Android）：影片播放器。上游的 mov 模块从来没实现过，这一层
// 是 rlvm-android 新加的，见 docs/DECISIONS.md D-027 与 dev-log/MOV-VIDEO-M3A.jsonl。
// 只做平台调用，不改动脚本语义。
#include "android/mov_player.h"
#include "android/app_log.h"
#include "machine/general_operations.h"
#include "machine/long_operation.h"
#include "machine/rlmachine.h"
#include "machine/rloperation.h"
#include "machine/rloperation/default_value.h"
#include "systems/base/system.h"
#include "systems/base/text_system.h"

namespace {

// 脚本里写的是影片名（LBEX / Kud Wafter 都是 `movPlayEx("OP00", 0, 0, 799, 599)`），
// 实际文件在 MOV/ 目录下、扩展名 .mpg。
std::string MovieFileId(const std::string& name) { return "MOV/" + name + ".mpg"; }

// 影片起播前把「快进 / 跳过」复位（真机反馈 v0.2.4）：
// 开着 auto/skip 时，影片播放期间脚本里的 `wait` 会被快进直接跳掉，剧情就跟着影片
// 一起往前跑（表现为"影片播放没有中断游戏"）。影片本身就是要「等它放完」，
// 所以在这里把这两种省时模式关掉。
void ResetTimeSaversBeforeMovie(RLMachine& machine) {
  // Skip 与 Auto 都是引擎侧状态（TextSystem::skip_mode_ / auto_mode_，
  // 由 /sys:ClearSkipMode、/sys:SetAutoMode 设置）。影片开播前把两个都清掉：
  // 否则影片一被"点击跳过"打断，游戏会带着 Auto/Skip 状态继续一口气快进到
  // 下一章（真机反馈）。用户口径：宁可播完再手动重新打开这两个模式。
  machine.system().text().SetSkipMode(0);
  machine.system().text().SetAutoMode(false);
  machine.system().clear_force_fast_forward();
  // 再补一刀：光清标志还不够——引擎跑字节码是「按时间片成批执行」的（一个片最多
  // 10ms，skip 状态下能冲过一整段剧情，真机表现是"影片开始了剧情还往后走了一截，
  // 停在一章开头"）。这里打一个帧边界（脚本 refresh() 用的是同一机制），
  // 让当前这批指令到此为止，剧本就停在影片调用的这一条上。
  machine.system().set_force_wait(true);
}

// movPlayEx(name, x, y, w, h)：异步起播，脚本继续往下走（实测脚本后面接 wait()）。
typedef RLOp_Void_5<StrConstant_T, IntConstant_T, IntConstant_T, IntConstant_T,
                    IntConstant_T>
    MovPlaySignature;

struct MovPlayEx : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    const bool ok = rlvm_android::MovPlayer::Instance().Play(MovieFileId(name),
                                                             x, y, w, h);
    ResetTimeSaversBeforeMovie(machine);
    // 指令级 trace（v0.2.4 查「影片播放时游戏没被中断」）：看脚本到底调了哪条、
    // 有没有真的起播。结果同时进应用日志（面板「日志」里能看）。
    rlvm_android::AppendAppLogLine(
        "mov-op: movPlayEx(" + name + ") 起播" + (ok ? "成功" : "失败"));
  }
};

// movPlay(0)：目标游戏里没有调用点，暂按与 movPlayEx 相同的参数形状实现，
// 等遇到真实调用点再校正（写进 dev-log）。
struct MovPlay : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    const bool ok = rlvm_android::MovPlayer::Instance().Play(MovieFileId(name),
                                                             x, y, w, h);
    ResetTimeSaversBeforeMovie(machine);
    rlvm_android::AppendAppLogLine(
        "mov-op: movPlay(" + name + ") 起播" + (ok ? "成功" : "失败"));
  }
};

// movStop(5)：停播。
struct MovStop : public RLOp_Void_Void {
  void operator()(RLMachine& machine) {
    const bool was = rlvm_android::MovPlayer::Instance().playing();
    rlvm_android::MovPlayer::Instance().Stop();
    rlvm_android::AppendAppLogLine(std::string("mov-op: movStop（之前") +
                                   (was ? "在播" : "没在播") + "）");
  }
};

// movWait(3)：等影片放完。实现成 LongOperation——每次过游戏循环问一次播放器，
// 放完（或本来就)就返回，脚本继续。
// 等影片放完的长操作；timeout_ms > 0 时到点也返回（movWait 的超时参数）。
class MovWaitLongOperation : public LongOperation {
 public:
  explicit MovWaitLongOperation(int timeout_ms) : timeout_ms_(timeout_ms) {
    if (timeout_ms > 0) {
      deadline_ms_ =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count() +
          timeout_ms;
    }
  }

  bool operator()(RLMachine& machine) override {
    if (!rlvm_android::MovPlayer::Instance().playing()) {
      if (!logged_done_) {
        logged_done_ = true;
        rlvm_android::AppendAppLogLine("mov-op: 等待结束（影片已停止）——脚本继续");
      }
      return true;
    }
    if (timeout_ms_ > 0) {
      const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      if (now >= deadline_ms_) {
        if (!logged_done_) {
          logged_done_ = true;
          rlvm_android::AppendAppLogLine("mov-op: 等待结束（超时 " +
                                         std::to_string(timeout_ms_) +
                                         "ms）——脚本继续");
        }
        return true;  // 到点：不再等，脚本继续
      }
    }
    return false;
  }

 private:
  int timeout_ms_ = 0;
  long long deadline_ms_ = 0;
  bool logged_done_ = false;
};

// 参数是可选的等待上限（毫秒）；0/缺省 = 一直等到放完。
struct MovWait : public RLOp_Void_1<DefaultIntValue_T<0>> {
  void operator()(RLMachine& machine, int timeout_ms) {
    if (!rlvm_android::MovPlayer::Instance().playing()) {
      rlvm_android::AppendAppLogLine(
          "mov-op: movWait(" + std::to_string(timeout_ms) + ") 没在播，直接返回");
      return;
    }
    rlvm_android::AppendAppLogLine(
        "mov-op: movWait(" + std::to_string(timeout_ms) + ") 压入等待");
    machine.PushLongOperation(new MovWaitLongOperation(timeout_ms));
  }
};

/**
 * movPlayExC(20)：起播并**等它放完**才让脚本继续。
 *
 * 依据：KW/LBEX 的 SEEN514 里这一段之后紧跟 `wait(2200)` + ShowCursor——只有当
 * ExC 阻塞到影片结束，这个顺序才说得通；否则剧情会在 OP 还挂在屏幕上时往下走
 * （真机上"自测和主页面并行"就是这种感觉，那是因为自测不受脚本控制）。
 * 播放中点击可以跳过：native 侧停掉播放器，这个长操作下一轮就返回 true。
 *
 * 注：C 后缀的准确含义尚未证实（可能是 clear / complete），按「播完才继续」实现。
 */
struct MovPlayExC : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    rlvm_android::MovPlayer& player = rlvm_android::MovPlayer::Instance();
    ResetTimeSaversBeforeMovie(machine);
    const bool ok = player.Play(MovieFileId(name), x, y, w, h);
    if (ok && player.playing()) {
      rlvm_android::AppendAppLogLine(
          "mov-op: movPlayExC(" + name + ") 起播并压入等待（脚本应停在这里）");
      machine.PushLongOperation(new MovWaitLongOperation(/*timeout_ms=*/0));
    } else {
      rlvm_android::AppendAppLogLine(
          "mov-op: movPlayExC(" + name + ") 未起播，不等待（检查文件/解码器）");
    }
  }
};

// movPlaying(4)：把「是否在播」写进目标变量。
struct MovPlaying : public RLOp_Void_1<IntReference_T> {
  void operator()(RLMachine& machine, IntReferenceIterator dest) {
    *dest = rlvm_android::MovPlayer::Instance().playing() ? 1 : 0;
  }
};

/**
 * movLoop(2)：循环播放（放完从头再来，直到 movStop）。
 * 目标游戏里没有调用点，参数形状按与 movPlayEx 一致实现（写进 dev-log）。
 */
struct MovLoop : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    const bool ok = rlvm_android::MovPlayer::Instance().Play(
        MovieFileId(name), x, y, w, h, 0, /*loop=*/true);
    ResetTimeSaversBeforeMovie(machine);
    rlvm_android::AppendAppLogLine(
        "mov-op: movLoop(" + name + ") 起播" + (ok ? "成功（循环）" : "失败"));
  }
};

}  // namespace

MovModule::MovModule() : RLModule("Mov", 1, 26) {
  AddOpcode(0, 0, "movPlay", new MovPlay);
  AddOpcode(1, 0, "movPlayEx", new MovPlayEx);
  AddOpcode(2, 0, "movLoop", new MovLoop);
  AddOpcode(3, 0, "movWait", new MovWait);
  AddOpcode(4, 0, "movPlaying", new MovPlaying);
  AddOpcode(5, 0, "movStop", new MovStop);
  AddOpcode(20, 0, "movPlayExC", new MovPlayExC);
}
