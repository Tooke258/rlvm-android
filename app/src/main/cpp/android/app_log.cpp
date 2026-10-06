#include "android/app_log.h"

#include <android/log.h>

#include <cstdio>
#include <mutex>
#include <sstream>
#include <vector>

namespace rlvm_android {
namespace {

std::mutex g_app_log_mutex;
std::string g_app_log_path;
bool g_lb_wipe_log = false;
std::vector<std::string> g_lb_op_trace_filter;

}  // namespace

bool LbWipeLogEnabled() { return g_lb_wipe_log; }

void SetLbWipeLog(bool on) { g_lb_wipe_log = on; }

void SetLbOpTraceFilter(const std::string& csv_substrings) {
  g_lb_op_trace_filter.clear();
  std::string cur;
  for (char c : csv_substrings) {
    if (c == ',') {
      if (!cur.empty()) g_lb_op_trace_filter.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) g_lb_op_trace_filter.push_back(cur);
}

bool LbOpTraceWanted(const std::string& op_name) {
  if (g_lb_op_trace_filter.empty()) return true;
  for (const std::string& s : g_lb_op_trace_filter) {
    if (op_name.find(s) != std::string::npos) return true;
  }
  return false;
}

namespace {
bool g_lb_case_trace = false;
}  // namespace

void SetLbCaseTrace(bool on) { g_lb_case_trace = on; }

bool LbCaseTraceWanted() { return g_lb_case_trace; }

namespace {
bool g_lb_patno_trace = false;
}  // namespace

void SetLbPatNoTrace(bool on) { g_lb_patno_trace = on; }

bool LbPatNoTraceWanted() { return g_lb_patno_trace; }

namespace {
int g_lb_last_scene = -1;
int g_lb_last_line = -1;
std::string g_lb_last_op;
}  // namespace

void SetLbLastOpContext(int scene, int line, const std::string& op_name) {
  g_lb_last_scene = scene;
  g_lb_last_line = line;
  g_lb_last_op = op_name;
}

std::string LbLastOpContextString() {
  if (g_lb_last_scene < 0) return "(none)";
  std::ostringstream oss;
  oss << "SEEN" << g_lb_last_scene << " L" << g_lb_last_line << " "
      << g_lb_last_op;
  return oss.str();
}

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
