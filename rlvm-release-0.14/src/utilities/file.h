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

#ifndef SRC_UTILITIES_FILE_H_
#define SRC_UTILITIES_FILE_H_

#include <boost/filesystem.hpp>

#include <iosfwd>
#include <string>
#include <vector>

class Gameexe;
class RLMachine;
class System;

// On platforms with case-insensitive file systems, returns a copy of the input
// unchanged. On less tolerant platforms, returns a copy of the input with
// correct case, or the empty string if no solution could be found.
boost::filesystem::path CorrectPathCase(boost::filesystem::path Path);

// Reads the entire contents of a file into the passed in |data| and
// |size|. Returns true if there were no problems.
bool LoadFileData(const boost::filesystem::path& path,
                  std::unique_ptr<char[]>& fileData,
                  int& fileSize);

// Android/SAF 移植新增：按"游戏文件标识"打开只读 fd 的钩子。
//
// 普通路径后端下标识就是路径，直接 fopen 即可；而 SAF 下标识是不透明的，
// 只有 Kotlin 侧（ContentResolver）能把它换成 fd。语音归档（KOE/NWK/OVK/koepac）
// 目前用 fopen/ifstream 按真实路径读文件，在 SAF 下必然失败，因此提供这个钩子：
// 平台层安装实现，未安装时返回 -1，调用方自行回退到普通路径。
typedef int (*OpenGameFileFdHook)(const char* file_id);
void SetOpenGameFileFdHook(OpenGameFileFdHook hook);
int OpenGameFileFd(const std::string& file_id);

// Android/SAF 移植新增：写入变体。按"游戏文件标识"创建（或截断）文件并返回
// 可写 fd；未安装钩子或无此能力时返回 -1，调用方回退到普通路径。
typedef int (*OpenGameFileWriteFdHook)(const char* file_id);
void SetOpenGameFileWriteFdHook(OpenGameFileWriteFdHook hook);
int OpenGameFileWriteFd(const std::string& file_id);

// 把一段数据写到"游戏文件"：优先走写钩子（SAF 下唯一可行），失败则回退到
// 普通 ofstream。存档与全局数据（Config）都经这里落盘。
void WriteGameFile(const boost::filesystem::path& path, const std::string& data);

// 读取整个"游戏文件"到内存：优先走读钩子（SAF 下唯一可行），失败回退普通
// 路径。存档与全局数据的**读取**都用它——只改写入是不够的。
bool ReadGameFileAll(const boost::filesystem::path& path, std::string& out);

// 判断"游戏文件"是否存在：优先用读钩子试着打开（SAF 下唯一可行），
// 失败回退 boost::filesystem::exists。存档菜单用它判断槽位是否占用。
bool GameFileExists(const boost::filesystem::path& path);

// Android 移植新增：把存档目录从默认的 $HOME/.rlvm/<REGNAME> 改到指定位置。
// 传空路径表示恢复默认。设置后 System::GameSaveDirectory() 直接返回它，
// 并且不再尝试用 boost 去创建目录（SAF 下没有真实路径，创建由平台层负责）。
void SetGameSaveDirectoryOverride(const boost::filesystem::path& path);
boost::filesystem::path GetGameSaveDirectoryOverride();

#endif  // SRC_UTILITIES_FILE_H_
