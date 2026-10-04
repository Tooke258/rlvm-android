// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2009 Elliot Glaysher
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
// -----------------------------------------------------------------------

#ifndef SRC_MACHINE_GAME_HACKS_H__
#define SRC_MACHINE_GAME_HACKS_H__

class RLMachine;

// Adds game specific hacks that execute at certain seen/line pairs.
void AddGameHacks(RLMachine& machine);

// Little Busters / LBEX 的棒球小游戏实现塞在 PT00.dll 里（RLVM 尚未逆向）。
// 默认与上游一致：在那个 seen/line 上直接 ReturnFromFarcall（跳过小游戏）。
// 传 false 则**不跳过**，让脚本继续跑进小游戏流程——逆向 PT00 时用它来收集
// "脚本到底调用了哪些 DLL func、参数是什么"（配合 little_busters_pt00dll.cc 的调用记录）。
void SetLBSkipBaseball(bool skip);

#endif  // SRC_MACHINE_GAME_HACKS_H__
