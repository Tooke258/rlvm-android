#include "android/app_log.h"

#include <android/log.h>

#include <cstdio>
#include <mutex>

namespace rlvm_android {
namespace {

std::mutex g_app_log_mutex;
std::string g_app_log_path;

}  // namespace

void SetAppLogFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_app_log_mutex);
  g_app_log_path = path;
}

void AppendAppLogLine(const std::string& line) {
  __android_log_print(ANDROID_LOG_INFO, "rlvm-applog", "%s", line.c_str());
  std::lock_guard<std::mutex> lock(g_app_log_mutex);
  if (g_app_log_path.empty()) return;
  FILE* f = std::fopen(g_app_log_path.c_str(), "a");
  if (f == nullptr) return;
  std::fprintf(f, "%s\n", line.c_str());
  std::fclose(f);
}

}  // namespace rlvm_android
