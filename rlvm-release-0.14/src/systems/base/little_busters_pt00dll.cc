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

#include "systems/base/little_busters_pt00dll.h"

#include <iostream>

#include "machine/rlmachine.h"

LittleBustersPT00DLL::LittleBustersPT00DLL() {
  std::cerr << "WARNING: Little Busters Baseball is implemented in a DLL and "
            << "hasn't been reverse engineered yet." << std::endl;
}

LittleBustersPT00DLL::~LittleBustersPT00DLL() {}

int LittleBustersPT00DLL::CallDLL(RLMachine& machine,
                                  int func,
                                  int arg1,
                                  int arg2,
                                  int arg3,
                                  int arg4) {
  // 调用记录（逆向 PT00 的第一步）：把每次调用原样打出来，用来枚举"脚本到底用了哪些
  // func、参数长什么样、按什么顺序"。输出走 std::cerr —— Android 上由 log_redirect
  // 接到 logcat，因此这里不依赖任何平台 API。
  //
  // 平时看不到这些输出：只有把棒球小游戏的跳过关掉（诊断文件里 lb_minigame=1，
  // 见 machine/game_hacks.h 的 SetLBSkipBaseball）脚本才会跑进小游戏、进而调用到这里。
  std::cerr << "PT00 call: (SEEN" << machine.SceneNumber() << ")(Line "
            << machine.line_number() << ") func=" << func << " args=[" << arg1
            << ", " << arg2 << ", " << arg3 << ", " << arg4 << "]" << std::endl;
  return 0;
}

const std::string& LittleBustersPT00DLL::GetDLLName() const {
  static std::string n("PT00");
  return n;
}
