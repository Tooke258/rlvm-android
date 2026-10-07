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

#include "long_operations/button_object_select_long_operation.h"

#include "machine/rlmachine.h"
#include "android/app_log.h"
#include "systems/base/graphics_object.h"
#include "systems/base/graphics_system.h"
#include "systems/base/parent_graphics_object_data.h"
#include "systems/base/system.h"

ButtonObjectSelectLongOperation::ButtonObjectSelectLongOperation(
    RLMachine& machine,
    int group)
    : machine_(machine),
      group_(group),
      cancelable_(false),
      has_return_value_(false),
      return_value_(-1),
      gameexe_(machine.system().gameexe()),
      currently_hovering_button_(NULL),
      currently_pressed_button_(NULL) {
  GraphicsSystem& graphics = machine.system().graphics();
  for (GraphicsObject& obj : graphics.GetForegroundObjects()) {
    if (obj.IsButton() && obj.GetButtonGroup() == group_) {
      buttons_.push_back(
          std::make_pair(&obj, static_cast<GraphicsObject*>(NULL)));
    } else if (obj.has_object_data()) {
      ParentGraphicsObjectData* parent =
          dynamic_cast<ParentGraphicsObjectData*>(&obj.GetObjectData());

      if (parent) {
        for (GraphicsObject& child : parent->objects()) {
          if (child.IsButton() && child.GetButtonGroup() == group_) {
            buttons_.push_back(std::make_pair(&child, &obj));
          }
        }
      }
    }
  }

  // Initialize overrides on all buttons that we'll use.
  for (ButtonPair& button_pair : buttons_) {
    SetButtonOverride(button_pair.first, "NORMAL");
  }
}

ButtonObjectSelectLongOperation::~ButtonObjectSelectLongOperation() {
  // Disable overrides on all graphics objects we've dealt with.
  for (ButtonPair& button_pair : buttons_) {
    button_pair.first->ClearButtonOverrides();
    // 移植补丁（2026-10-07，用户定调）：关菜单后按钮"残留在原地"。
    //
    // 现场：SEEN7800 的关闭路径只做
    //     loop (intL[0] < 14): objVisible(221, 1+intL[0], 0)
    // 也就是**只隐藏子对象 1..14**；而暂停菜单的底图/按钮落在 221 的
    // **#0 / #15 / #16 / #17**（渲染树实测：CAM 菜单那 15 项仍挂在同一个父对象下，
    // 子对象列表在换菜单时没有被清空），那条循环永远扫不到它们 ⇒ 关菜单后仍然 vis=1。
    //
    // 这里在长操作析构（= 菜单真正结束）时，把它管辖的按钮对象一并置为不可见。
    // 属于引擎侧、窄口径的偏离：脚本下次要用它们时会重新 objVisible/objOfChild 建回来。
    button_pair.first->SetVisible(0);
    // 底板：渲染树实测，暂停菜单的底板 `PT_RMENU_BG00` 是**同一个父对象下的 child #0**
    // （它与按钮 15/16/17 同父），不是按钮、因而不在 buttons_ 名单里 ——
    // 只隐按钮会"按钮没了、底板还在"。这里把同父的 0 号子对象一并隐藏。
    GraphicsObject* parent = button_pair.second;
    if (parent && parent->has_object_data()) {
      ParentGraphicsObjectData* parent_data =
          dynamic_cast<ParentGraphicsObjectData*>(&parent->GetObjectData());
      if (parent_data) {
        LazyArray<GraphicsObject>& kids = parent_data->objects();
        LazyArray<GraphicsObject>::full_iterator it = kids.full_begin();
        if (it != kids.full_end() && it.valid()) it->SetVisible(0);
      }
    }
  }
}

void ButtonObjectSelectLongOperation::MouseMotion(const Point& point) {
  GraphicsObject* hovering_button = NULL;

  for (ButtonPair& button_pair : buttons_) {
    if (button_pair.first->has_object_data()) {
      GraphicsObjectData* data = &button_pair.first->GetObjectData();
      Rect screen_rect = data->DstRect(*button_pair.first, button_pair.second);

      if (screen_rect.Contains(point))
        hovering_button = button_pair.first;
    }
  }

  if (currently_hovering_button_ != hovering_button) {
    if (currently_hovering_button_) {
      SetButtonOverride(currently_hovering_button_, "NORMAL");

      if (currently_hovering_button_ == currently_pressed_button_)
        currently_pressed_button_ = NULL;
    }

    if (hovering_button)
      SetButtonOverride(hovering_button, "HIT");
  }

  currently_hovering_button_ = hovering_button;
}

bool ButtonObjectSelectLongOperation::MouseButtonStateChanged(
    MouseButton mouseButton,
    bool pressed) {
  if (mouseButton == MOUSE_LEFT) {
    if (pressed) {
      currently_pressed_button_ = currently_hovering_button_;
      if (currently_pressed_button_)
        SetButtonOverride(currently_pressed_button_, "PUSH");
    } else {
      if (currently_hovering_button_ &&
          currently_hovering_button_ == currently_pressed_button_) {
        has_return_value_ = true;
        return_value_ = currently_pressed_button_->GetButtonNumber();
        SetButtonOverride(currently_pressed_button_, "HIT");
      }
    }

    // Changes override properties doesn't automatically refresh the screen the
    // way mouse movement does.
    machine_.system().graphics().ForceRefresh();

    return true;
  } else if (mouseButton == MOUSE_RIGHT && !pressed && cancelable_) {
    has_return_value_ = true;
    return_value_ = -1;
  }

  return false;
}

bool ButtonObjectSelectLongOperation::operator()(RLMachine& machine) {
  if (has_return_value_) {
    machine.set_store_register(return_value_);
    return true;
  } else {
    return false;
  }
}

void ButtonObjectSelectLongOperation::SetButtonOverride(GraphicsObject* object,
                                                        const char* type) {
  // 试验开关（diag: no_button_overrides=1）：完全不应用覆盖 ⇒ 对象渲染用脚本写的
  // `patt_no_`，而不是 GAMEEXE.INI 的 BTNOBJ.ACTION 表。见 android/app_log.h。
  if (rlvm_android::LbNoButtonOverridesWanted()) return;
  int action = object->GetButtonAction();

  GameexeInterpretObject key = gameexe_("BTNOBJ.ACTION", action, type);
  if (key.Exists()) {
    const std::vector<int>& ints = key.ToIntVector();
    object->SetButtonOverrides(ints[0], ints[2], ints[3]);
  }
}
