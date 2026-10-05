// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2013 Elliot Glaysher
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
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
// -----------------------------------------------------------------------

#ifndef SRC_SYSTEMS_BASE_LITTLE_BUSTERS_PT00DLL_H_
#define SRC_SYSTEMS_BASE_LITTLE_BUSTERS_PT00DLL_H_

#include <string>

#include "machine/reallive_dll.h"

// Little Busters 的棒球小游戏（PT00.dll）。
//
// 这里按 dev-log/PT00-RECON.jsonl 与 docs/PT00-CALLSITES.md 的取证结果做
// 「行为级重写」（路线 A）：DLL 不画屏幕、不读输入，只通过引擎的 intD 区与脚本
// 交换数据，因此移植只需要读写 intD。每个 func 的语义都在实现里逐条注明出处。
class LittleBustersPT00DLL : public RealLiveDLL {
 public:
  LittleBustersPT00DLL();
  virtual ~LittleBustersPT00DLL();

  // Overridden from RealLiveDLL:
  virtual int CallDLL(RLMachine& machine,
                      int func,
                      int arg1,
                      int arg2,
                      int arg3,
                      int arg4) override;
  virtual const std::string& GetDLLName() const override;

  // 诊断（rlvm-diag.txt 的 lb_minigame=1，平台层设置）：把每次 CallDLL 的
  // func 与四个参数打进来，用来在真机上观察脚本是怎么驱动 PT00 的。
  static void SetCallLogging(bool enabled);

 private:
  // func 930/931 用的是 DLL 自己的一个小状态块（ptr[1]/ptr[2]），不是 intD。
  int scene_flag_ = -1;
  int scene_counter_ = 0;
  // func 31（主推进）用的 DLL 内部字段（原型的 state 结构体 +1204/+1208/+1212）。
  bool hit_pending_ = false;
  int hit_timer_ = 0;
  int hit_counter_ = 0;
};

#endif  // SRC_SYSTEMS_BASE_LITTLE_BUSTERS_PT00DLL_H_
