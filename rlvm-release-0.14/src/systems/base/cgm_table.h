// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2008 Elliot Glaysher
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

#ifndef SRC_SYSTEMS_BASE_CGM_TABLE_H_
#define SRC_SYSTEMS_BASE_CGM_TABLE_H_

#include <boost/serialization/access.hpp>
#include <map>
#include <set>
#include <string>

class Gameexe;
class RLMachine;

// Manages whether a certain CG was viewed.
//
// The cgm file is a mapping between file names and an index into the intZ[]
// global memory array, where intZ[index] is 1 when a cg has been viewed. The
// CGMTable class is responsible for loading the cgm data and providing an
// interface to querrying whether a CG was viewed.
class CGMTable {
 public:
  // Initializes an empty CG table (for games that don't use this feature).
  CGMTable();

  // Initializes the CG table with the CGM data file specified in the
  // #CGTABLE_FILENAME gameexe key.
  explicit CGMTable(Gameexe& gameexe);
  ~CGMTable();

  // Returns the total number of images designated as CGs.
  int GetTotal() const;

  // Returns the number of CG entries viewed.
  int GetViewed() const;

  // Returns the percentage of CG images that have been viewed.
  int GetPercent() const;

  // Returns the cg index for filename, or -1 if filename is not a CG image.
  int GetFlag(const std::string& filename) const;

  // Returns a value indicating whether filename is a CG that has been viewed:
  //   1   CG has been viewed
  //   0   CG has not been viewed
  //  -1   filename is not a CG image
  int GetStatus(const std::string& filename) const;

  // Mark a cg as viewed. Sets intZ[getFlag()] to 1.
  void SetViewed(RLMachine& machine, const std::string& filename);

  // 移植辅助（Android）：把 CG 表里的**所有**条目都标记成"已观看"。
  // 语义与 SetViewed 完全一致（intZ[flag]=1 + cgm_data_.insert(flag)），
  // 但逐个条目容错：flag 越界（RLVM 的 intZ 只有 2000 个槽）时只计数不抛，
  // 这样引擎不会因为表里某个越界条目而整轮中断。返回一行人类可读报告。
  //
  // 为什么需要它：相册场景（SEEN9515 / LBEX 的"CG 鉴赏"）用 Sys 1504
  // （cgStatus）逐条问"这个文件看过没有"，而 cgStatus 读的就是 cgm_data_。
  // 也就是说 cgm_data_ 才是"收集度"的真源，而它只在真正显示过 CG 时被填。
  std::string MarkAllViewed(RLMachine& machine);

  // 诊断：把 CG 表内容 dump 成 "文件名=flag" 的多行文本（最多 200 条）。
  std::string DumpTable() const;

 private:
  typedef std::map<std::string, int> CGMMap;

  // Mapping between a graphics file name and the file's cg index.
  CGMMap cgm_info_;

  // When a CG is viewed, its index is added to this set. This data is
  // considered global and persists through interpreter invocations.
  std::set<int> cgm_data_;

  // boost::serialization support
  friend class boost::serialization::access;
  template <class Archive>
  void serialize(Archive& ar, unsigned int version) {
    // We don't save |cgm_info_|. That will be the same during each run since
    // it's read from a game data file.
    ar& cgm_data_;
  }
};  // end of class CGMTable

#endif  // SRC_SYSTEMS_BASE_CGM_TABLE_H_
