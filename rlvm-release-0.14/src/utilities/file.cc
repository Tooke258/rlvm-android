// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of RLVM, a RealLive virtual machine clone.
//
// -----------------------------------------------------------------------
//
// Copyright (C) 2006 Elliot Glaysher
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

#include "utilities/file.h"

#include "android/game_file_system.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

#include <unistd.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <vector>
#include <string>

#include "systems/base/system.h"
#include "systems/base/system_error.h"
#include "utilities/exception.h"

using boost::to_upper;
using std::stack;
using std::ostringstream;
using std::string;
using std::ifstream;
using std::ios;

namespace fs = boost::filesystem;

// -----------------------------------------------------------------------

fs::path CorrectPathCase(fs::path Path) {
#ifndef CASE_SENSITIVE_FILESYSTEM
  if (!fs::exists(Path))
    return path();
  return Path;
#else
  // If the path is OK as it stands, do nothing.
  if (fs::exists(Path))
    return Path;
  // If the path doesn't seem to be OK, track backwards through it
  // looking for the point at which the problem first arises.  Path
  // will contain the parts of the path that exist on the current
  // filesystem, and pathElts will contain the parts after that point,
  // which may have incorrect case.
  std::stack<std::string> pathElts;
  while (!Path.empty() && !fs::exists(Path)) {
    pathElts.push(Path.filename().string());
    Path = Path.parent_path();
  }
  // Now proceed forwards through the possibly-incorrect elements.
  while (!pathElts.empty()) {
    // Does this element need to be a directory?
    // (If we are searching for /foo/bar/baz, and /foo contains a file
    // bar and a directory Bar, then we need to know which is the one
    // we're looking for.  This will still be unreliable if /foo
    // contains directories bar and Bar, but a full backtracking
    // search would be complicated; for now this should be adequate!)
    const bool needDir = pathElts.size() > 1;
    std::string elt(pathElts.top());
    pathElts.pop();
    // Does this element exist?
    if (exists(Path / elt) && (!needDir || is_directory(Path / elt))) {
      // If so, use it.
      Path /= elt;
    } else {
      // If not, search for a suitable candidate.
      to_upper(elt);
      fs::directory_iterator end;
      bool found = false;
      for (fs::directory_iterator dir(Path); dir != end; ++dir) {
        std::string uleaf = dir->path().filename().string();
        to_upper(uleaf);
        if (uleaf == elt && (!needDir || is_directory(*dir))) {
          Path /= dir->path().filename();
          found = true;
          break;
        }
      }
      if (!found)
        return "";
    }
  }
  return Path.string();
#endif
}

// -----------------------------------------------------------------------

bool LoadFileData(const boost::filesystem::path& path,
                  std::unique_ptr<char[]>& fileData,
                  int& fileSize) {
  // Android 移植：优先经过游戏文件系统读取。
  // SAF 后端下 path 是游戏相对路径（没有真实文件系统路径可用）；
  // 普通路径后端下相对与绝对路径都能正确解析，因此这里不再直接用 fs::ifstream。
  std::shared_ptr<rlvm_android::GameFileSystem> game_files =
      rlvm_android::GetGameFileSystem();
  if (game_files) {
    std::vector<char> contents;
    if (!rlvm_android::ReadGameFile(path.string(), contents)) {
      ostringstream oss;
      oss << "Could not open file \"" << path << "\".";
      throw rlvm::Exception(oss.str());
    }
    fileSize = static_cast<int>(contents.size());
    fileData.reset(new char[fileSize > 0 ? fileSize : 1]);
    if (fileSize > 0)
      std::memcpy(fileData.get(), contents.data(), fileSize);
    // 与上游语义一致：返回 !ifs.good()，成功时为 false。
    return false;
  }

  fs::ifstream ifs(path, ifstream::in | ifstream::binary);
  if (!ifs) {
    ostringstream oss;
    oss << "Could not open file \"" << path << "\".";
    throw rlvm::Exception(oss.str());
  }

  ifs.seekg(0, ios::end);
  fileSize = ifs.tellg();
  ifs.seekg(0, ios::beg);

  fileData.reset(new char[fileSize]);
  ifs.read(fileData.get(), fileSize);

  return !ifs.good();
}

// -----------------------------------------------------------------------
// Android/SAF 移植新增：游戏文件 fd 钩子（见 file.h 的说明）。
// -----------------------------------------------------------------------

namespace {

OpenGameFileFdHook g_open_game_file_fd_hook = NULL;

}  // namespace

void SetOpenGameFileFdHook(OpenGameFileFdHook hook) {
  g_open_game_file_fd_hook = hook;
}

int OpenGameFileFd(const std::string& file_id) {
  if (g_open_game_file_fd_hook == NULL) return -1;
  return g_open_game_file_fd_hook(file_id.c_str());
}

OpenGameFileWriteFdHook g_open_game_file_write_fd_hook = NULL;

void SetOpenGameFileWriteFdHook(OpenGameFileWriteFdHook hook) {
  g_open_game_file_write_fd_hook = hook;
}

int OpenGameFileWriteFd(const std::string& file_id) {
  if (g_open_game_file_write_fd_hook == NULL) return -1;
  return g_open_game_file_write_fd_hook(file_id.c_str());
}

void WriteGameFile(const boost::filesystem::path& path, const std::string& data) {
  // SAF 下没有真实路径，只能靠平台钩子；普通路径后端没有钩子，直接写文件。
  const int fd = OpenGameFileWriteFd(path.string());
  if (fd >= 0) {
    size_t written = 0;
    while (written < data.size()) {
      const ssize_t n = write(fd, data.data() + written, data.size() - written);
      if (n <= 0) break;
      written += static_cast<size_t>(n);
    }
    close(fd);
    if (written == data.size()) return;
  }

  boost::filesystem::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw rlvm::Exception("Could not open file for writing: " + path.string());
  }
  file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

namespace {

boost::filesystem::path g_game_save_directory_override;

}  // namespace

void SetGameSaveDirectoryOverride(const boost::filesystem::path& path) {
  g_game_save_directory_override = path;
}

boost::filesystem::path GetGameSaveDirectoryOverride() {
  return g_game_save_directory_override;
}

bool ReadGameFileAll(const boost::filesystem::path& path, std::string& out) {
  out.clear();
  const int fd = OpenGameFileFd(path.string());
  if (fd >= 0) {
    char buffer[16 * 1024];
    for (;;) {
      const ssize_t n = read(fd, buffer, sizeof(buffer));
      if (n <= 0) break;
      out.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return !out.empty();
  }

  std::ifstream file(path.string(), std::ios::binary);
  if (!file) return false;
  out.assign((std::istreambuf_iterator<char>(file)),
             std::istreambuf_iterator<char>());
  return true;
}
