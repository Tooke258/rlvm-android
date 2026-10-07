// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2006, 2007 Elliot Glaysher
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

#include "machine/rlmodule.h"

#include <iomanip>
#include <iostream>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

#include "libreallive/bytecode.h"
#include "libreallive/intmemref.h"
#include "machine/general_operations.h"
#include "machine/rloperation.h"
#include "utilities/exception.h"
#include "android/app_log.h"

// -----------------------------------------------------------------------
// RLMoudle
// -----------------------------------------------------------------------

RLModule::RLModule(const std::string& in_module_name,
                   int in_module_type,
                   int in_module_number)
    : property_list_(),
      module_type_(in_module_type),
      module_number_(in_module_number),
      module_name_(in_module_name) {}

RLModule::~RLModule() {}

int RLModule::PackOpcodeNumber(int opcode, unsigned char overload) {
  return ((int)opcode << 8) | overload;
}

void RLModule::UnpackOpcodeNumber(int packed_opcode,
                                  int& opcode,
                                  unsigned char& overload) {
  opcode = (packed_opcode >> 8);
  overload = packed_opcode & 0xFF;
}

void RLModule::AddOpcode(int opcode,
                         unsigned char overload,
                         const std::string& name,
                         RLOperation* op) {
  int packed_opcode = PackOpcodeNumber(opcode, overload);
  op->set_name(name);
  op->module_ = this;
#ifndef NDEBUG
  OpcodeMap::iterator it = stored_operations_.find(packed_opcode);

  if (it != stored_operations_.end()) {
    std::ostringstream oss;
    oss << "Duplicate opcode in " << *this << ": opcode " << opcode << ", "
        << int(overload);
    throw rlvm::Exception(oss.str());
  }
#endif
  stored_operations_.insert(
      std::make_pair(packed_opcode, std::unique_ptr<RLOperation>(op)));
}

void RLModule::AddUnsupportedOpcode(int opcode,
                                    unsigned char overload,
                                    const std::string& name) {
  AddOpcode(opcode,
            overload,
            name,
            new UndefinedFunction(
                module_type_, module_number_, opcode, (int)overload));
}

void RLModule::SetProperty(int property, int value) {
  if (!property_list_) {
    property_list_.reset(new std::vector<std::pair<int, int>>);
  }

  // Modify the property if it already exists
  PropertyList::iterator it = FindProperty(property);
  if (it != property_list_->end()) {
    it->second = value;
    return;
  }

  property_list_->push_back(std::make_pair(property, value));
}

bool RLModule::GetProperty(int property, int& value) const {
  if (property_list_) {
    PropertyList::iterator it = FindProperty(property);
    if (it != property_list_->end()) {
      value = it->second;
      return true;
    }
  }

  return false;
}

RLModule::PropertyList::iterator RLModule::FindProperty(int property) const {
  return find_if(property_list_->begin(),
                 property_list_->end(),
                 [&](Property& p) { return p.first == property; });
}

void RLModule::DispatchFunction(RLMachine& machine,
                                const libreallive::CommandElement& f) {
  OpcodeMap::iterator it =
      stored_operations_.find(PackOpcodeNumber(f.opcode(), f.overload()));
  if (it != stored_operations_.end()) {
    try {
      if (machine.is_tracing_on()) {
        // 平台侧白名单过滤器（diag: op_trace=...）。过滤器为空时行为与原来完全一致，
        // 只影响「打印哪几条」，不碰分发/异常语义。
        if (rlvm_android::LbOpTraceWanted(it->second->name())) {
          std::cerr << "(SEEN" << std::setw(4) << std::setfill('0')
                    << machine.SceneNumber()
                    << ")(Line " << std::setw(4) << std::setfill('0')
                    << machine.line_number() << "): " << it->second->name();
          libreallive::PrintParameterString(std::cerr,
                                            f.GetUnparsedParameters());
          std::cerr << std::endl;
        }
      }
      // 取证：记住「最近一条派发的指令」，供 graphics_object 的
      // `[params-reset]` 日志标注是哪条指令之后发生的参数重置。
      // 死循环探测（diag: loop_detect=1）：只统计最近若干条 (场景,行号)，
      // 一旦按周期重复就报一次，用于"卡死但不崩"（例如相册 Scene 回想）。
      rlvm_android::NoteOpForLoopDetect(machine.SceneNumber(),
                                        machine.line_number());
      if (rlvm_android::LbPatNoTraceWanted()) {
        rlvm_android::SetLbLastOpContext(machine.SceneNumber(),
                                         machine.line_number(),
                                         it->second->name());
      }
      // 相册页码取证（diag: gallery_probe=1）：见 android/app_log.h 的说明。
      //
      // **不猜行号**：第一版用 L278/L301/L304/L307/L310 白名单，结果真机上
      // 一行都没打——因为守卫没过时 L303-311 整块根本不派发，而 `if` 在字节码里
      // 也未必落在 301 这一行。改成"值一变就打"：只要 SEEN9515 里
      // intL[11]/intL[20]/intL[21] 变化就出一行，附带当时的行号与页码表取值。
      if (rlvm_android::LbGalleryProbeWanted() &&
          machine.SceneNumber() == 9515) {
        using libreallive::IntMemRef;
        const int l0 = machine.GetIntValue(IntMemRef(libreallive::INTL_LOCATION, 0));
        const int l11 = machine.GetIntValue(IntMemRef(libreallive::INTL_LOCATION, 11));
        const int l20 = machine.GetIntValue(IntMemRef(libreallive::INTL_LOCATION, 20));
        const int l21 = machine.GetIntValue(IntMemRef(libreallive::INTL_LOCATION, 21));
        static int p11 = -99999, p20 = -99999, p21 = -99999;
        if (l11 != p11 || l20 != p20 || l21 != p21) {
          p11 = l11;
          p20 = l20;
          p21 = l21;
          std::ostringstream g;
          g << "[gallery] SEEN9515 L" << machine.line_number() << " op="
            << it->second->name() << " intL[0]=" << l0 << " intL[11]=" << l11
            << " intL[20]=" << l20 << " intL[21]=" << l21
            << " intA[7200+intL[11]]="
            << machine.GetIntValue(IntMemRef(libreallive::INTA_LOCATION, 7200 + l11));
          rlvm_android::AppendAppLogLine(g.str());
        }
      }
      it->second->DispatchFunction(machine, f);
    }
    catch (rlvm::Exception& e) {
      e.setOperation(it->second.get());
      throw;
    }
  } else {
    throw rlvm::UnimplementedOpcode(machine, f);
  }
}

std::ostream& operator<<(std::ostream& os, const RLModule& module) {
  os << "mod<" << module.module_name() << "," << module.module_type() << ":"
     << module.module_number() << ">";
  return os;
}
