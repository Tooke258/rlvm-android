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

// 平台相关实现（Android）：影片播放器。上游的 mov 模块从来没实现过，这一层
// 是 rlvm-android 新加的，见 docs/DECISIONS.md D-027 与 dev-log/MOV-VIDEO-M3A.jsonl。
// 只做平台调用，不改动脚本语义。
#include "android/mov_player.h"
#include "machine/general_operations.h"
#include "machine/long_operation.h"
#include "machine/rlmachine.h"
#include "machine/rloperation.h"
#include "machine/rloperation/default_value.h"

namespace {

// 脚本里写的是影片名（LBEX / Kud Wafter 都是 `movPlayEx("OP00", 0, 0, 799, 599)`），
// 实际文件在 MOV/ 目录下、扩展名 .mpg。
std::string MovieFileId(const std::string& name) { return "MOV/" + name + ".mpg"; }

// movPlayEx(name, x, y, w, h)：异步起播，脚本继续往下走（实测脚本后面接 wait()）。
typedef RLOp_Void_5<StrConstant_T, IntConstant_T, IntConstant_T, IntConstant_T,
                    IntConstant_T>
    MovPlaySignature;

struct MovPlayEx : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    rlvm_android::MovPlayer::Instance().Play(MovieFileId(name), x, y, w, h);
  }
};

// movPlay(0)：目标游戏里没有调用点，暂按与 movPlayEx 相同的参数形状实现，
// 等遇到真实调用点再校正（写进 dev-log）。
struct MovPlay : public MovPlaySignature {
  void operator()(RLMachine& machine, std::string name, int x, int y, int w,
                  int h) {
    rlvm_android::MovPlayer::Instance().Play(MovieFileId(name), x, y, w, h);
  }
};

// movStop(5)：停播。
struct MovStop : public RLOp_Void_Void {
  void operator()(RLMachine& machine) {
    rlvm_android::MovPlayer::Instance().Stop();
  }
};

// movWait(3)：等影片放完。实现成 LongOperation——每次过游戏循环问一次播放器，
// 放完（或本来就)就返回，脚本继续。
class MovWaitLongOperation : public LongOperation {
 public:
  bool operator()(RLMachine& machine) override {
    return !rlvm_android::MovPlayer::Instance().playing();
  }
};

// 参数是可选的等待上限（毫秒）；0/缺省 = 一直等到放完。
struct MovWait : public RLOp_Void_1<DefaultIntValue_T<0>> {
  void operator()(RLMachine& machine, int timeout_ms) {
    (void)timeout_ms;  // TODO(v0.2.3)：需要超时语义时再实现
    if (!rlvm_android::MovPlayer::Instance().playing()) return;
    machine.PushLongOperation(new MovWaitLongOperation);
  }
};

// movPlaying(4)：把「是否在播」写进目标变量。
struct MovPlaying : public RLOp_Void_1<IntReference_T> {
  void operator()(RLMachine& machine, IntReferenceIterator dest) {
    *dest = rlvm_android::MovPlayer::Instance().playing() ? 1 : 0;
  }
};

}  // namespace

MovModule::MovModule() : RLModule("Mov", 1, 26) {
  AddOpcode(0, 0, "movPlay", new MovPlay);
  AddOpcode(1, 0, "movPlayEx", new MovPlayEx);
  AddUnsupportedOpcode(2, 0, "movLoop");
  AddOpcode(3, 0, "movWait", new MovWait);
  AddOpcode(4, 0, "movPlaying", new MovPlaying);
  AddOpcode(5, 0, "movStop", new MovStop);
  AddOpcode(20, 0, "movPlayExC", new MovPlayEx);
}
