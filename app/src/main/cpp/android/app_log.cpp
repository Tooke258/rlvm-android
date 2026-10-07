#include "android/app_log.h"

#include <android/log.h>

#include <cstdio>
#include <chrono>
#include <mutex>
#include <sstream>
#include <vector>

namespace rlvm_android {
namespace {

std::mutex g_app_log_mutex;
std::string g_app_log_path;
bool g_lb_wipe_log = false;
std::vector<std::string> g_lb_op_trace_filter;
// 逐指令 trace 的"预算"：-1 = 不限；>0 表示还能打这么多行；0 = 停止打印。
// 用途：全量 trace（op_trace=*）时防止日志被上百万行冲爆。
long g_lb_op_trace_budget = -1;

}  // namespace

bool LbWipeLogEnabled() { return g_lb_wipe_log; }

void SetLbWipeLog(bool on) { g_lb_wipe_log = on; }

void SetLbOpTraceFilter(const std::string& csv_substrings) {
  g_lb_op_trace_filter.clear();
  // "*" = 不过滤（打全部），便于抓"卡在哪个循环"。
  if (csv_substrings == "*") return;
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
  if (g_lb_op_trace_budget == 0) return false;
  if (g_lb_op_trace_budget > 0) --g_lb_op_trace_budget;
  if (g_lb_op_trace_filter.empty()) return true;
  for (const std::string& s : g_lb_op_trace_filter) {
    if (op_name.find(s) != std::string::npos) return true;
  }
  return false;
}

void SetLbOpTraceBudget(long n) { g_lb_op_trace_budget = n; }

namespace {

// 死循环探测：按 (场景, 行号) 计数，每 2 秒把当前最热的两行写进应用日志。
// 为什么不是"发现重复就报一次"：正常的刷帧循环（例如相册自己的更新循环）
// 在探测眼里和死循环同形，只报一次会被它占住，反而看不到真正卡住的位置。
// 改成时间序列后：日志里最后那条（以及它是否在变）就是答案——
//   * 点击前一直是 SEEN9515/0127（相册刷帧，正常）
//   * 点击后如果卡死，最后几行会稳定指向同一个"卡住的那一行"
struct LoopProbe {
  bool enabled = false;
  bool started = false;
  long long total = 0;
  long long last_report_ms = 0;
  static const int kSlots = 32;
  int scn[kSlots];
  int line[kSlots];
  long long cnt[kSlots];

  static long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
  }

  void Note(int s, int l) {
    if (!enabled) return;
    ++total;

    int free_slot = -1;
    int worst = 0;
    for (int i = 0; i < kSlots; ++i) {
      if (cnt[i] == 0) {
        free_slot = i;
        break;
      }
      if (cnt[i] < cnt[worst]) worst = i;
    }
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) {
      if (cnt[i] > 0 && scn[i] == s && line[i] == l) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = free_slot >= 0 ? free_slot : worst;
      scn[slot] = s;
      line[slot] = l;
      cnt[slot] = 0;
    }
    ++cnt[slot];

    const long long now = NowMs();
    if (!started) {
      started = true;
      last_report_ms = now;
      return;
    }
    if (now - last_report_ms < 2000) return;
    last_report_ms = now;

    int top1 = -1, top2 = -1;
    for (int i = 0; i < kSlots; ++i) {
      if (cnt[i] == 0) continue;
      if (top1 < 0 || cnt[i] > cnt[top1]) {
        top2 = top1;
        top1 = i;
      } else if (top2 < 0 || cnt[i] > cnt[top2]) {
        top2 = i;
      }
    }
    char buf[256];
    if (top1 < 0) {
      snprintf(buf, sizeof(buf), "loop_probe: 2s idle (total=%lld)", total);
    } else if (top2 < 0) {
      snprintf(buf, sizeof(buf),
               "loop_probe: 2s top=(SEEN%d)(Line %d) n=%lld (total=%lld)",
               scn[top1], line[top1], cnt[top1], total);
    } else {
      snprintf(buf, sizeof(buf),
               "loop_probe: 2s top=(SEEN%d)(Line %d) n=%lld ; 2nd=(SEEN%d)"
               "(Line %d) n=%lld (total=%lld)",
               scn[top1], line[top1], cnt[top1], scn[top2], line[top2],
               cnt[top2], total);
    }
    AppendAppLogLine(buf);
    std::fprintf(stderr, "%s\n", buf);
    for (int i = 0; i < kSlots; ++i) cnt[i] = 0;
  }
};

LoopProbe g_loop_probe;

}  // namespace

void SetLoopDetector(bool on) {
  g_loop_probe.enabled = on;
  g_loop_probe.started = false;
  for (int i = 0; i < LoopProbe::kSlots; ++i) {
    g_loop_probe.scn[i] = 0;
    g_loop_probe.line[i] = 0;
    g_loop_probe.cnt[i] = 0;
  }
}

void NoteOpForLoopDetect(int scene, int line) {
  g_loop_probe.Note(scene, line);
}

namespace {
bool g_longop_log = false;
}  // namespace

void SetLongOpLog(bool on) { g_longop_log = on; }

bool LongOpLogWanted() { return g_longop_log; }

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
bool g_lb_gallery_probe = false;
}  // namespace

void SetLbGalleryProbe(bool on) { g_lb_gallery_probe = on; }

bool LbGalleryProbeWanted() { return g_lb_gallery_probe; }

namespace {
bool g_lb_wipe_copy_all = false;
}  // namespace

void SetLbWipeCopyAll(bool on) { g_lb_wipe_copy_all = on; }

bool LbWipeCopyAllWanted() { return g_lb_wipe_copy_all; }

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
