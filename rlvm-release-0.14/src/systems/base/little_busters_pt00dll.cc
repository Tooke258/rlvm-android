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

#include <cmath>
#include <iostream>
#include <string>

#include "libreallive/intmemref.h"
#include "machine/rlmachine.h"

using libreallive::IntMemRef;

namespace {

// 诊断开关：平台层读 rlvm-diag.txt 后调用 SetCallLogging()。
bool g_log_calls = false;
int g_call_count = 0;

int GetD(RLMachine& machine, int index) {
  return machine.GetIntValue(IntMemRef(libreallive::INTD_LOCATION, index));
}

void SetD(RLMachine& machine, int index, int value) {
  machine.SetIntValue(IntMemRef(libreallive::INTD_LOCATION, index), value);
}

// PT00 的实体记录表：intD[1000 + i * 36 + k]（i = 0..21）。
// 依据：DLL 里所有实体访问都是 `*(intD_base + 4040 + i * 144 + k * 4)`，
// 4040 / 4 = 1010 = 1000 + 10，而脚本自己也直接读写 intD[1000 + i*36 + k]。
const int kEntBase = 1000;
const int kEntStride = 36;
const int kEntCount = 22;

int Ent(RLMachine& machine, int i, int field) {
  return GetD(machine, kEntBase + i * kEntStride + field);
}

// 3x3 行主序矩阵乘列向量（PT00.dll sub_10002700）。
void MatVec(const double* m, const double* v, double* out) {
  out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
  out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
  out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

void LogCall(int func, int a1, int a2, int a3, int a4, const char* note);

// ---------------------------------------------------------------------------
// func 10 / 11：3D 投影（sub_10002780 / sub_100029D0）
//
// 先把点 {x, y, z} 平移 {0, -2000, -1000}，再绕 X 轴转 -25°，然后做透视除法：
//   intD[1900] = 620 - (int)(X * -850 / Z)
//   intD[1901] = 176 - (int)(Y *  400 / Z)
//   intD[1902] = (int)Z
// 另外用 y 与 w 各算一次，把两次的高度差写进 intD[1903] / intD[1904]（立绘高度）。
// 三个参考点分别是 {x, 0, z}、{x, y, z}、{x, w, z}。
// 反编译里先建了 Z(0)、Y(0)、X(-25°) 三张旋转矩阵并连乘，前两张是单位阵，
// 所以最终只剩 X 轴 -25° 的旋转。
// ---------------------------------------------------------------------------
void ProjectBillboard(RLMachine& machine, int x, int y, int z, int w) {
  const double kAngle = -0.4363323129861111;  // -25 度
  const double c = std::cos(kAngle);
  const double s = std::sin(kAngle);
  // sub_10002540 构造的 X 轴旋转矩阵（行主序）。
  const double m[9] = {1.0, 0.0, 0.0, 0.0, c, -s, 0.0, s, c};

  double r[3];
  auto project = [&](double py) {
    const double v[3] = {static_cast<double>(x), py - 2000.0,
                         static_cast<double>(z) - 1000.0};
    MatVec(m, v, r);
  };

  project(0.0);
  const int base_y = 176 - static_cast<int>(r[1] * 400.0 / r[2]);
  SetD(machine, 1900, 620 - static_cast<int>(r[0] * -850.0 / r[2]));
  SetD(machine, 1901, base_y);
  SetD(machine, 1902, static_cast<int>(r[2]));

  project(static_cast<double>(y));
  SetD(machine, 1903, 176 - static_cast<int>(r[1] * 400.0 / r[2]) - base_y);

  project(static_cast<double>(w));
  SetD(machine, 1904, 176 - static_cast<int>(r[1] * 400.0 / r[2]) - base_y);
}

// func 12：直接换算（sub_10002A00）。脚本拿 intD[1900]/[1901] 当屏幕偏移、
// intD[1903]/[1904] 当缩放。
void SetScreenFromWorld(RLMachine& machine, int x, int y, int z, int w) {
  SetD(machine, 1900, x / 10 + 1300);
  SetD(machine, 1901, 1100 - z / 10);
  SetD(machine, 1902, 0);
  SetD(machine, 1903, y / -10);
  SetD(machine, 1904, w / -10);
}

// func 60：场地横向边界 clamp（sub_10004380）。结果写 intD[320] 并返回，
// 脚本随即把 intD[320] 当 func 12 的第 4 个参数传回来。
int ClampFieldX(RLMachine& machine, int a2, int a3) {
  int v9 = 0;
  if (a2 < 0) {
    const int v11 = -a3 - a2 / 2;
    if (v11 >= 280) v9 = (v11 < 1255) ? (v11 - 280) / 3 : 325;
  } else {
    const int v8 = a2 / 2 - a3;
    if (v8 >= 1240 && v8 < 2260) {
      v9 = 310;
    } else if (v8 >= 310 && v8 < 1240) {
      v9 = (v8 - 310) / 3;
    } else if (v8 >= 2260 && v8 < 4113) {
      v9 = (3190 - v8) / 3;
    } else if (v8 >= 4523) {
      v9 = -567;
    } else if (v8 >= 4113) {
      v9 = -307;
    }
  }
  SetD(machine, 320, v9);
  return v9;
}

// func 61：选角色 / 装姿态（sub_10004470）。intD[351] 非 0 时忽略（除非 force）。
void PickCharacter(RLMachine& machine, int a2, int a3, bool force) {
  if (GetD(machine, 351) != 0 && !force) return;

  SetD(machine, 350, a2);
  SetD(machine, 355, GetD(machine, 352));
  SetD(machine, 356, GetD(machine, 353));
  SetD(machine, 357, GetD(machine, 354));
  SetD(machine, 361, 0);
  SetD(machine, 362, a3);
  for (int i = 0; i < kEntCount; ++i) {
    if (a2 == i + 10) {
      SetD(machine, 358, Ent(machine, i, 10));
      SetD(machine, 359, Ent(machine, i, 11));
      SetD(machine, 360, Ent(machine, i, 12));
    }
  }
  switch (a2) {
    case 1:
      SetD(machine, 358, GetD(machine, 256));
      SetD(machine, 359, GetD(machine, 257));
      SetD(machine, 360, GetD(machine, 258));
      break;
    case 3:
      SetD(machine, 358, GetD(machine, 1835));
      SetD(machine, 359, GetD(machine, 1836));
      SetD(machine, 360, GetD(machine, 1837));
      break;
    case 4:
      SetD(machine, 358, GetD(machine, 1852));
      SetD(machine, 359, GetD(machine, 1853));
      SetD(machine, 360, GetD(machine, 1854));
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// func 900/901 是打者/投球，func 910/911 是球 A，func 920/921 是球 B，
// func 930/931 是场次计数。四组都是「0/2 入口 = init，entrypoint 3 = 每帧 step」。
// ---------------------------------------------------------------------------

// func 920：球 B 复位（sub_10014B40）。扫 13..21 号实体，第一个指令号为 18 的
// 就是这颗球对应的实体，把它摆到 (-1000, -1000, -1000)。
void BallBReset(RLMachine& machine) {
  SetD(machine, 1850, 0);
  SetD(machine, 1851, 0);
  SetD(machine, 1852, 0);
  SetD(machine, 1853, 0);
  SetD(machine, 1854, 0);
  SetD(machine, 1855, GetD(machine, 1852));
  SetD(machine, 1856, GetD(machine, 1853));
  SetD(machine, 1857, GetD(machine, 1854));
  SetD(machine, 1861, 0);
  SetD(machine, 1858, 2000);
  SetD(machine, 1859, 0);
  SetD(machine, 1860, 0);
  SetD(machine, 1862, 0);
  SetD(machine, 1863, 0);
  SetD(machine, 1864, 0);
  SetD(machine, 1865, 0);

  for (int i = 13; i < kEntCount; ++i) {
    if (Ent(machine, i, 2) == 18) {
      SetD(machine, 1850, 1);
      SetD(machine, 1851, 1);
      SetD(machine, 1852, -1000);
      SetD(machine, 1853, -1000);
      SetD(machine, 1854, -1000);
      SetD(machine, 1855, -1000);
      SetD(machine, 1856, -1000);
      SetD(machine, 1857, -1000);
      break;
    }
  }
}

// func 921：球 B 每帧（sub_10014C60）。抛物线积分 + 落地反弹；落地时把
// intD[1855..1857] 记为新的起点、把 t 清零、把 intD[1864] 置 1（命中标记）。
void BallBStep(RLMachine& machine) {
  if (GetD(machine, 1850) != 1) return;

  SetD(machine, 1863, GetD(machine, 1863) + 1);
  const int t = GetD(machine, 1863);
  SetD(machine, 1852,
       GetD(machine, 1855) + GetD(machine, 1858) * GetD(machine, 1861) * t / 10 /
                               1000);
  SetD(machine, 1853,
       static_cast<int>(static_cast<double>(GetD(machine, 1856) +
                                            t * GetD(machine, 1862) / 10) -
                        static_cast<double>(t * t) * 0.6));
  SetD(machine, 1854,
       GetD(machine, 1857) + GetD(machine, 1858) * GetD(machine, 1860) * t / 10 /
                               1000);

  if (ClampFieldX(machine, GetD(machine, 1852), GetD(machine, 1854)) > 0) {
    // 出界：整颗球失效。
    SetD(machine, 1850, 0);
    SetD(machine, 1851, 0);
    return;
  }
  if (GetD(machine, 1853) >= 0) return;

  SetD(machine, 1861, GetD(machine, 1861) + -4 - GetD(machine, 1862) / 20);
  if (GetD(machine, 1861) < 0) SetD(machine, 1861, 0);
  SetD(machine, 1862,
       static_cast<int>((static_cast<double>(GetD(machine, 1862)) * 0.1 -
                         static_cast<double>(GetD(machine, 1863)) * 1.2) *
                        -0.7 * 10.0));
  SetD(machine, 1853, 0);
  SetD(machine, 1855, GetD(machine, 1852));
  SetD(machine, 1856, GetD(machine, 1853));
  SetD(machine, 1857, GetD(machine, 1854));
  SetD(machine, 1863, 0);
  SetD(machine, 1864, 1);
}

// func 910：球 A 复位（sub_10014970）。
void BallAReset(RLMachine& machine) {
  const bool active = GetD(machine, 1039) == 8 && GetD(machine, 1111) == 8;
  SetD(machine, 1830, active ? 1 : 0);
  SetD(machine, 1835, active ? GetD(machine, 1118) : 0);
  SetD(machine, 1836, active ? GetD(machine, 1119) : 0);
  SetD(machine, 1837, active ? GetD(machine, 1120) : 0);
  SetD(machine, 1831, 0);
  SetD(machine, 1832, GetD(machine, 1835));
  SetD(machine, 1833, GetD(machine, 1836));
  SetD(machine, 1834, GetD(machine, 1837));
  SetD(machine, 1839, 0);
  SetD(machine, 1840, 0);
  SetD(machine, 1841, 0);
}

// func 30：全局复位（sub_10002AA0）里对 intD 的那部分。
// 反编译里还有一次对 DLL 内部状态块的 memset，那些字段目前只有 func 31 会读，
// 而 func 31 还没移植，所以先只做 intD。
void ResetState(RLMachine& machine) {
  SetD(machine, 210, 0);
  SetD(machine, 220, 0);
  SetD(machine, 221, 255);
  for (int i = 222; i <= 246; ++i) SetD(machine, i, 0);
  SetD(machine, 250, 0);
  SetD(machine, 251, 255);
  for (int i = 252; i <= 281; ++i) SetD(machine, i, 0);
}

void LogCall(int func, int a1, int a2, int a3, int a4, const char* note) {
  if (!g_log_calls) return;
  ++g_call_count;
  // 前 300 次逐条打，之后每 60 次打一条，免得每帧十几次调用把日志刷爆。
  if (g_call_count > 300 && (g_call_count % 60) != 0) return;
  std::cout << "[pt00] #" << g_call_count << " f=" << func << " a=(" << a1 << ","
            << a2 << "," << a3 << "," << a4 << ")";
  if (note != nullptr) std::cout << " " << note;
  std::cout << std::endl;
}

}  // namespace

void LittleBustersPT00DLL::SetCallLogging(bool enabled) { g_log_calls = enabled; }

LittleBustersPT00DLL::LittleBustersPT00DLL() {
  std::cerr << "NOTE: Little Busters Baseball (PT00) is a behaviour-level "
            << "reimplementation of the original DLL; see docs/PT00-CALLSITES.md"
            << std::endl;
}

LittleBustersPT00DLL::~LittleBustersPT00DLL() {}

int LittleBustersPT00DLL::CallDLL(RLMachine& machine,
                                  int func,
                                  int arg1,
                                  int arg2,
                                  int arg3,
                                  int arg4) {
  switch (func) {
    case 10:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      ProjectBillboard(machine, arg1, arg2, arg3, arg4);
      break;
    case 11:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      ProjectBillboard(machine, arg1 / 2, arg2, arg3, arg4);
      break;
    case 12:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      SetScreenFromWorld(machine, arg1, arg2, arg3, arg4);
      break;
    case 30:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      ResetState(machine);
      scene_flag_ = -1;
      scene_counter_ = 0;
      break;
    case 60:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      ClampFieldX(machine, arg1, arg2);
      break;
    case 61:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      PickCharacter(machine, arg1, arg2, arg3 == 1);
      break;
    case 910:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      BallAReset(machine);
      break;
    case 920:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      BallBReset(machine);
      break;
    case 921:
      BallBStep(machine);
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      break;
    case 930:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      scene_flag_ = -1;
      scene_counter_ = 0;
      break;
    case 931:
      LogCall(func, arg1, arg2, arg3, arg4, nullptr);
      // 反编译 sub_10014F50：this[1] 为 0 且计数为 0 时写 intD[30] = 7
      // （场景切换信号），计数 30 时调一次收尾，然后计数 +1。
      if (scene_flag_ == 0 && scene_counter_ == 0) SetD(machine, 30, 7);
      ++scene_counter_;
      break;
    default:
      LogCall(func, arg1, arg2, arg3, arg4, "(not implemented yet; ignored)");
      break;
  }
  // 反编译 reallive_dll_func_call：每条出口都返回 1，脚本也不接收返回值。
  return 1;
}

const std::string& LittleBustersPT00DLL::GetDLLName() const {
  static std::string n("PT00");
  return n;
}
