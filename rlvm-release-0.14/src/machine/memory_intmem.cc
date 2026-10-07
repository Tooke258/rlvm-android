// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (c) 2004-2007  Kazunori "jagarl" Ueno
// Copyright (C) 2007 Elliot Glaysher
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. The name of the author may not be used to endorse or promote products
//    derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// -----------------------------------------------------------------------
//
// This file contains memory functions that were taken from xclannad,
// and are thus under the 3-clause BSD instead of the GPL used in the
// rest of rlvm.
//
// -----------------------------------------------------------------------

#include <sstream>
#include <string>

#include "machine/memory.h"
#include "machine/rlmachine.h"
#include "android/app_log.h"
#include "utilities/exception.h"
#include "libreallive/intmemref.h"

using libreallive::IntMemRef;

namespace {

// Helper function that throws errors for illegal memory access
void throwIllegalIndex(const IntMemRef& ref, const std::string& function) {
  std::ostringstream ss;
  ss << "Invalid memory access " << ref << " in " << function;
  throw rlvm::Exception(ss.str());
}

void saveOriginalValue(int* bank,
                       std::map<int, int>* original_bank,
                       int location) {
  if (bank && original_bank) {
    std::map<int, int>::iterator it = original_bank->find(location);
    if (it == original_bank->end()) {
      original_bank->insert(std::make_pair(location, bank[location]));
    }
  }
}

}  // namespace

int Memory::GetIntValue(const IntMemRef& ref) {
  int type = ref.type();
  int index = ref.bank();
  int location = ref.location();

  int* bank = NULL;
  if (index == 8) {
    bank = machine_.CurrentIntLBank();
  } else if (index < 0 || index > NUMBER_OF_INT_LOCATIONS) {
    throwIllegalIndex(ref, "RLMachine::GetIntValue()");
  } else {
    bank = int_var[index];
  }

  if (type == 0) {
    // A[]..G[], Z[] を直に読む
    // **这里以前写死 2000**（与 memory.h 的 SIZE_OF_MEM_BANK 无关）。LBEX 的脚本
    // 会用到高位区间（相册 `SEEN9515` 的页码表就是 `intA[900*8+n]` = `intA[7200+n]`），
    // 写死 2000 会把它们全部拒掉 ⇒ 读出来是垃圾 ⇒ 页码位只能画 0 号图
    // （真机症状：UI 先正确渲染一瞬、随后整体复位默认）。改成跟随存储区实际大小。
    if ((unsigned int)(location) >= (unsigned int)SIZE_OF_MEM_BANK)
      throwIllegalIndex(ref, "RLMachine::GetIntValue()");

    return bank[location];
  } else {
    // Ab[]..G4b[], Z8b[] などを読む
    int factor = 1 << (type - 1);
    int eltsize = 32 / factor;
    if ((unsigned int)(location) >=
        ((unsigned int)SIZE_OF_MEM_BANK * 32u / (unsigned int)factor))
      throwIllegalIndex(ref, "RLMachine::GetIntValue()");

    return (bank[location / eltsize] >> ((location % eltsize) * factor)) &
           ((1 << factor) - 1);
  }
}

void Memory::SetIntValue(const IntMemRef& ref, int value) {
  int type = ref.type();
  int index = ref.bank();
  int location = ref.location();

  int* bank = NULL;
  std::map<int, int>* original_bank = NULL;
  if (index == 8) {
    bank = machine_.CurrentIntLBank();
  } else if (index < 0 || index > NUMBER_OF_INT_LOCATIONS) {
    throwIllegalIndex(ref, "RLMachine::SetIntValue()");
  } else {
    bank = int_var[index];
    original_bank = original_int_var[index];
  }

  if (type == 0) {
    // A[]..G[], Z[] を直に書く（同上：以前这里写死 2000）
    if ((unsigned int)(location) >= (unsigned int)SIZE_OF_MEM_BANK)
      throwIllegalIndex(ref, "RLMachine::SetIntValue()");

    // 取证（diag: gallery_probe=1）：盯 LBEX 相册的 CG 页表 intA[7200..7259]。
    // 只在这个区间、且值真变化时打一行，附带"最近一条派发的指令"上下文——
    // 用来回答：这张表是**谁建的**、又是**谁清的**（真机症状：先渲染正确、随后复位默认）。
    if (rlvm_android::LbGalleryProbeWanted() && index == 0 && location >= 7200 &&
        location < 7260) {
      static int last_val[60];
      static bool last_init = false;
      const int slot = location - 7200;
      if (!last_init) {
        for (int i = 0; i < 60; ++i) last_val[i] = 0x7fffffff;
        last_init = true;
      }
      if (last_val[slot] != value) {
        last_val[slot] = value;
        rlvm_android::AppendAppLogLine(
            "[cgtable] intA[" + std::to_string(location) + "]=" +
            std::to_string(value) + " after=" +
            rlvm_android::LbLastOpContextString());
      }
    }

    saveOriginalValue(bank, original_bank, location);
    bank[location] = value;
  } else {
    // Ab[]..G4b[], Z8b[] などを書く
    int factor = 1 << (type - 1);
    int eltsize = 32 / factor;
    int eltmask = (1 << factor) - 1;
    int shift = (location % eltsize) * factor;
    if ((unsigned int)(location) >=
        ((unsigned int)SIZE_OF_MEM_BANK * 32u / (unsigned int)factor))
      throwIllegalIndex(ref, "RLMachine::SetIntValue()");

    saveOriginalValue(bank, original_bank, location / eltsize);
    bank[location / eltsize] =
        (bank[location / eltsize] & ~(eltmask << shift)) | (value & eltmask)
                                                               << shift;
  }
}
