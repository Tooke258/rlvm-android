#include "pt00_emu_bridge.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "libreallive/intmemref.h"
#include "machine/rlmachine.h"
#include "systems/base/graphics_system.h"
#include "systems/base/system.h"
#include "utilities/file.h"

// 执行器本体（tools/pt00_emu/emu.c，以 PT00_EMU_LIBRARY 编进 librlvm）。
// 接口约定：intD 是唯一的共享内存，调用前后由这里搬运。
extern "C" {
int pt00_emu_load_image(const void* data, unsigned size);
int pt00_emu_call(int func, int a1, int a2, int a3, int a4);
int pt00_emu_last_hit_step_limit(void);
unsigned pt00_emu_heap_used(void);
void pt00_emu_set_intd(const int* src, unsigned count);
void pt00_emu_get_intd(int* dst, unsigned count);
void pt00_emu_set_trace_ctx(int on, int max_lines);
void pt00_emu_set_watch(unsigned eip, int max_lines);
}

namespace {

constexpr int kIntDCount = 2000;

bool g_tried = false;
bool g_ready = false;
bool g_tick31 = false;  // 试验开关：帧首替脚本补调 CallDLL(0,31)
// pt00_intg_hack=0 时不在首次调用时预置 intG[1900]/[1901]（交给脚本自己置位）。
bool g_intg_hack = true;
// 逐调用级别的诊断日志（相机回写、每 200 次调用的直方图等）。
// **默认关**：logcat 是同步 I/O，小游戏每帧几十次调用时这种日志会把游戏本身拖成
// 「越跑越慢」（实测），排查时再用 rlvm-diag.txt 里的 pt00_verbose=1 打开。
bool g_verbose = false;
std::vector<int> g_intd(kIntDCount);

int GetD(RLMachine& machine, int index) {
  return machine.GetIntValue(
      libreallive::IntMemRef(libreallive::INTD_LOCATION, index));
}

void SetD(RLMachine& machine, int index, int value) {
  machine.SetIntValue(libreallive::IntMemRef(libreallive::INTD_LOCATION, index),
                      value);
}

int GetG(RLMachine& machine, int index) {
  return machine.GetIntValue(
      libreallive::IntMemRef(libreallive::INTG_LOCATION, index));
}

int GetF(RLMachine& machine, int index) {
  return machine.GetIntValue(
      libreallive::IntMemRef(libreallive::INTF_LOCATION, index));
}

}  // namespace

namespace pt00emu {

bool Available() { return g_ready; }

void Reset() {
  g_tried = false;
  g_ready = false;
}

void SetTraceCtx(bool on) { pt00_emu_set_trace_ctx(on ? 1 : 0, 800); }

void SetTick31(bool on) { g_tick31 = on; }

void SetIntgHack(bool on) { g_intg_hack = on; }

void SetWatch(unsigned eip, int max_lines) {
  pt00_emu_set_watch(eip, max_lines);
}

void SetVerbose(bool on) { g_verbose = on; }

void DumpIntD(RLMachine& machine) {
  std::cerr << "[pt00] full intD[0..1999] (dump on demand), 16 per line:" << std::endl;
  std::ostringstream os;
  for (int i = 0; i < kIntDCount; ++i) {
    if (i % 16 == 0) {
      if (i > 0) std::cerr << os.str() << std::endl;
      os.str("");
      os << "  [" << i << "]";
    }
    os << " " << GetD(machine, i);
  }
  std::cerr << os.str() << std::endl;
}

bool CallDLL(RLMachine& machine, int func, int a1, int a2, int a3, int a4) {
  if (!g_tried) {
    g_tried = true;
    std::string bytes;
    if (!ReadGameFileAll("PT00.dll", bytes) || bytes.empty()) {
      std::cerr << "[pt00] 游戏目录里没有 PT00.dll，退回行为级重写" << std::endl;
      return false;
    }
    if (pt00_emu_load_image(bytes.data(), static_cast<unsigned>(bytes.size())) !=
        0) {
      std::cerr << "[pt00] 执行器装载 PT00.dll 失败，退回行为级重写" << std::endl;
      return false;
    }
    g_ready = true;
    // 试验（可证伪）：原引擎在小游戏期间会把 intG[1900]/[1901] 置 1 ——
    // 脚本 SEEN7110:0087/0154 就是靠这两个标志决定要不要进入「可操作阶段」，
    // 而 RLVM 的 LB_SkipBaseball hack 把整段小游戏（连同这点引擎侧胶水）绕过去了。
    // 这里先手工置位，验证「缺的就是这块」这个假设。
    if (g_intg_hack) {
      machine.SetIntValue(
          libreallive::IntMemRef(libreallive::INTG_LOCATION, 1900), 1);
      machine.SetIntValue(
          libreallive::IntMemRef(libreallive::INTG_LOCATION, 1901), 1);
    } else {
      std::cerr << "[pt00] pt00_intg_hack=0: leave intG[1900]/[1901] to the script"
                << std::endl;
    }
    std::cerr << "[pt00] 试验：把 intG[1900]/[1901] 置 1（原引擎的小游戏胶水）"
              << std::endl;
    std::cerr << "[pt00] 兼容层就绪：执行器直接跑原版 PT00.dll（" << bytes.size()
              << " 字节）" << std::endl;
  }
  if (!g_ready) return false;

  // 真机诊断：前 40 次调用打出来，用来确认脚本确实走到了 PT00，
  // 调用日志：实体 AI 循环（70/71/72）每帧几十条会把别的都淹掉 —— 之前只记前 40 条，
  // 结果整段日志全是 72/70，看不出「相机 func=12 到底有没有被调」。改成：
  //   1) 只记 func 不在 {70,71,72} 的调用（上限放宽到 400 条）；
  //   2) 顺带记一份 func 直方图，随帧快照/图形栈转储一起打出来。
  static int g_func_counts[4096] = {0};
  if (func >= 0 && func < 4096) ++g_func_counts[func];
  static int logged_other = 0;
  if (g_verbose && func != 70 && func != 71 && func != 72 && logged_other < 400) {
    ++logged_other;
    std::cerr << "[pt00] CallDLL func=" << func << " a=(" << a1 << "," << a2
              << "," << a3 << "," << a4 << ")" << std::endl;
  }
  auto DumpFuncHistogram = [&]() {
    std::ostringstream os;
    os << "[pt00] func histogram:";
    for (int i = 0; i < 4096; ++i)
      if (g_func_counts[i] > 0) os << " " << i << "x" << g_func_counts[i];
    std::cerr << os.str() << std::endl;
  };

  // 每 ~35 次调用（= 脚本一帧）打一次关键 intD 快照：用来判断 DLL 的状态机
  // 到底有没有推进（卡在小游戏时最需要看这个）。前 3 帧全打，之后每 60 帧一次。
  static int call_count = 0;
  static int frames_logged = 0;
  if (++call_count % 35 == 0) {
    if (frames_logged < 3 || (call_count / 35) % 60 == 0) {
      ++frames_logged;
      std::ostringstream os;
      os << "[pt00] frame " << (call_count / 35) << " intD[30]=" << GetD(machine, 30)
         << " [46]=" << GetD(machine, 46) << " [210]=" << GetD(machine, 210)
         << " [320]=" << GetD(machine, 320) << " [350]=" << GetD(machine, 350)
         << " | ball1800..1804=";
      for (int i = 1800; i <= 1804; ++i) os << GetD(machine, i) << ",";
      os << " | ballA1830..1841=";
      for (int i = 1830; i <= 1841; ++i) os << GetD(machine, i) << ",";
      os << " | ballB1850..1865=";
      for (int i = 1850; i <= 1865; ++i) os << GetD(machine, i) << ",";
      os << " | flags 700/710/734/740/771=" << GetD(machine, 700) << "/"
         << GetD(machine, 710) << "/" << GetD(machine, 734) << "/"
         << GetD(machine, 740) << "/" << GetD(machine, 771);
      // 相位号一族（小游戏首次/演示阶段就是靠它分流；MAD 里由 SEEN7030 从
      // farcall 参数 intL[2] 写入，所以这里必须看它们的真实值）。
      os << " | phase 70..95=";
      for (int i = 70; i <= 95; ++i) os << GetD(machine, i) << ",";
      // A2 锚点要用 intG（原生侧同样可读）——小游戏相位标志就在这两个上。
      // 相机：小游戏把世界坐标转屏幕全靠它；脚本从不写这几个槽，只有
      // CallDLL(func=12) 写（PC 真值：12(-6000,0,3100,0)->1900=700,1901=790；
      // 12(0,0,0,0)->1900=1300,1901=1100）。全 0 = 所有子对象的 dst 落到屏幕外。
      os << " | cam 1900..1904=";
      for (int i = 1900; i <= 1904; ++i) os << GetD(machine, i) << ",";
      // 小游戏的状态机（脚本 SEEN7340/7420/7450 的分支全靠这些）：
      //   600 = 投球相位（SEEN7420:101 goto_case(intD[600])）
      //   630 = 投球动画帧计数（到 60 置 600=1）；733 = 回合帧计数（到 180 置 730=1）
      //   740..746 = 攻守/结果标志；760 = 帧计数；770..772 = 回合开关；42/44/45 = 球数/结果
      os << " | stm 600=" << GetD(machine, 600) << " 610..613=";
      for (int i = 610; i <= 613; ++i) os << GetD(machine, i) << ",";
      os << " 620..625=";
      for (int i = 620; i <= 625; ++i) os << GetD(machine, i) << ",";
      os << " 630=" << GetD(machine, 630) << " 42/44/45=" << GetD(machine, 42)
         << "/" << GetD(machine, 44) << "/" << GetD(machine, 45);
      os << " 730..746=";
      for (int i = 730; i <= 746; ++i) os << GetD(machine, i) << ",";
      os << " 760=" << GetD(machine, 760);
      os << " 770..772=";
      for (int i = 770; i <= 772; ++i) os << GetD(machine, i) << ",";
      os << " 100/101/109=" << GetD(machine, 100) << "/" << GetD(machine, 101)
         << "/" << GetD(machine, 109);
      os << " | intG[1900..1901]=" << GetG(machine, 1900) << "," << GetG(machine, 1901);
      // 【阵容取证】小游戏的「进行中」标志与阵容都存在 intF：
      //   SEEN7030 存：intF[1930]=1; intF[1931]=intD[70]; intF[1932]=intD[75];
      //                intF[1940+i]=intD[1000+i*36+2]
      //   SEEN7500 恢复：if (intF[1930]==1) intD[1000+i*36+2]=intF[1940+i]
      // 若 intF 这边是 0/-1，实体 1/3/5 就不在场 → 投球永不触发（= 手机上
      // 「击球手稳定被打中」的根因候选）。
      os << " | intF[1930..1952]=";
      for (int i = 1930; i <= 1952; ++i) {
        os << GetF(machine, i) << ",";
      }
      os << " | ent(+0/+1/+2)=";
      for (int i = 0; i < 22; ++i) {
        os << GetD(machine, 1000 + i * 36 + 0) << "/"
           << GetD(machine, 1000 + i * 36 + 1) << "/"
           << GetD(machine, 1000 + i * 36 + 2) << " ";
      }
      // 实体模式（record[4]）与**世界坐标**（record[10..12]）。
      // 用途 1：「被打中的球朝背后飞」——launch_ball 的 intD[76]==1/3 分支算的是
      //         (目标物位置 − 投手位置) 归一化；那些位置就是 record[10..12]。
      // 用途 2：角色渲染位置不对时，先看这里是不是 0 / 是不是和 PC 一致。
      os << " | entmode=";
      for (int i = 0; i < 22; ++i) {
        if (GetD(machine, 1000 + i * 36 + 0) == 0) continue;
        os << i << ":" << GetD(machine, 1000 + i * 36 + 4) << ",";
      }
      std::cerr << os.str() << std::endl;
      os.str("");
      os << "[pt00] entpos=";
      for (int i = 0; i < 22; ++i) {
        if (GetD(machine, 1000 + i * 36 + 0) == 0) continue;
        os << i << ":(" << GetD(machine, 1000 + i * 36 + 10) << ","
           << GetD(machine, 1000 + i * 36 + 11) << ","
           << GetD(machine, 1000 + i * 36 + 12) << ") ";
      }
      // 棒球小游戏真正的球状态：脚本 objShow(203,0,intD[220]) / objShow(205,0,intD[250])
      // **拆成多行**：logcat 单行有长度上限，之前 ball1/ball2 被截断过。
      os << "[pt00] ball1 intD[210..232]=";
      for (int i = 210; i <= 232; ++i) os << GetD(machine, i) << ",";
      std::cerr << os.str() << std::endl;
      os.str("");

      os << "[pt00] ball2 intD[250..262]=";
      for (int i = 250; i <= 262; ++i) os << GetD(machine, i) << ",";
      std::cerr << os.str() << std::endl;
      os.str("");

      // 飞行参数族（方向/速度）：[234]=z 方向系数、[235]=速度系数、
      // [236..242]=定点中间量、[239]/[241]=累加器、[243..246]=落点/回弹。
      // 真机报「球飞反了」要看的就是这一组；PC 对照用
      // `tools/pt00_probe.exe <pid> --entities`（已同步打印同样的槽）。
      os << "[pt00] ballp intD[233..246]=";
      for (int i = 233; i <= 246; ++i) os << GetD(machine, i) << ",";
      std::cerr << os.str() << std::endl;
      os.str("");

      // 出手参数是从这几处推出来的：intD[76]=模式（决定 func 900/901 的分支）、
      // intD[312]/[500]=投球种类、intD[904]=连投修正、[610..613]=打者目标点。
      os << "[pt00] parm intD[76]=" << GetD(machine, 76)
         << " [312]=" << GetD(machine, 312) << " [500]=" << GetD(machine, 500)
         << " [904]=" << GetD(machine, 904) << " [610..613]=";
      for (int i = 610; i <= 613; ++i) os << GetD(machine, i) << ",";
      // 投手（实体 0）的 record[10..12]＝出手点，launch_ball 直接抄它。
      os << " ent0=(" << GetD(machine, 1010) << "," << GetD(machine, 1011) << ","
         << GetD(machine, 1012) << ")";
      std::cerr << os.str() << std::endl;
      DumpFuncHistogram();
    }
  }

  // intD 是 DLL 与脚本唯一的交换区：进来先把引擎的值灌给执行器，
  // 小游戏期间定期把图形栈转储出来（卡住时引擎不会走到退出路径里的 dump_graphics）：
  // 每 30 帧一次、最多 5 次，用来确认球/人物这些对象的 src/dst 矩形是不是 0×0。
  if (call_count % 35 == 0) {
    static int tree_dumps = 0;
    const int frame_no = call_count / 35;
    if (tree_dumps < 5 && frame_no % 30 == 0) {
      ++tree_dumps;
      std::ostringstream tree;
      machine.system().graphics().Refresh(&tree);
      std::cerr << "[pt00] graphics tree @frame " << frame_no << ":" << std::endl
                << tree.str() << std::endl;
    }
  }
  // func 直方图：默认只在需要时打（见 g_verbose）。
  if (g_verbose && call_count % 2000 == 0) DumpFuncHistogram();
  // 跑完再把改动写回引擎（只写真正变了的槽，省点 SetIntValue 的开销）。
  for (int i = 0; i < kIntDCount; ++i) g_intd[i] = GetD(machine, i);
  pt00_emu_set_intd(g_intd.data(), kIntDCount);
  int cam_before[5];
  for (int i = 0; i < 5; ++i) cam_before[i] = g_intd[1900 + i];
  // 试验（可用 pt00_tick31=1 开关 A/B）：原引擎每帧替脚本驱动 DLL 一次
  // 「每帧主推进」。脚本这段循环里从不调 31，但 PC 上抓到的真实帧形态里
  // 71×22 之后就是 31 —— 判定帧首用 `72(0)`（每帧第一个 AI 调用）标记。
  if (g_tick31 && func == 72 && a1 == 0) {
    pt00_emu_call(31, 0, 0, 0, 0);
    static int tick_logged = 0;
    if (tick_logged++ < 5) {
      std::cerr << "[pt00] 试验：帧首替脚本补调 CallDLL(0,31)" << std::endl;
    }
  }
  // 「DLL 内部死循环」取证：**调用前**记环（这样最后一条就是卡住的那一次调用）。
  struct Rec { int f, a1, a2, a3, a4; };
  static Rec ring[16];
  static int ring_n = 0;
  ring[ring_n++ % 16] = Rec{func, a1, a2, a3, a4};
  pt00_emu_call(func, a1, a2, a3, a4);
  {
    static int reported = 0;
    if (pt00_emu_last_hit_step_limit() && reported < 3) {
      ++reported;
      std::cerr << "[pt00] 步数上限：最近 " << (ring_n < 16 ? ring_n : 16)
                << " 次 CallDLL（旧->新），heap_used=" << pt00_emu_heap_used()
                << "B：";
      int total = ring_n < 16 ? ring_n : 16;
      for (int i = 0; i < total; ++i) {
        const Rec& r = ring[(ring_n - total + i) % 16];
        std::cerr << " f=" << r.f << "(" << r.a1 << "," << r.a2 << "," << r.a3
                  << "," << r.a4 << ")";
      }
      std::cerr << std::endl;
    }
  }
  pt00_emu_get_intd(g_intd.data(), kIntDCount);
  // 相机回写取证：脚本的 intD[1900..1904] 全靠 CallDLL(func=12) 写；
  // 这里分别报告「执行器里变了没有」与「写回脚本数组了没有」，把两种失败分开：
  //   emu 变了但脚本没变 → 兼容层的同步问题；emu 也没变 → DLL 本身没写（要看输入）。
  {
    static const int kCamFirst = 1900, kCamLast = 1904;
    bool emu_changed = false, script_changed = false;
    for (int i = kCamFirst; i <= kCamLast; ++i) {
      if (g_intd[i] != cam_before[i]) emu_changed = true;
      if (g_intd[i] != GetD(machine, i)) script_changed = true;
    }
    if ((emu_changed || script_changed) && g_verbose) {
      std::cerr << "[pt00] cam after func=" << func << " emu_changed="
                << (emu_changed ? 1 : 0) << " script_changed="
                << (script_changed ? 1 : 0) << " now=";
      for (int i = kCamFirst; i <= kCamLast; ++i) std::cerr << g_intd[i] << ",";
      std::cerr << std::endl;
    }
  }
  for (int i = 0; i < kIntDCount; ++i) {
    if (g_intd[i] != GetD(machine, i)) SetD(machine, i, g_intd[i]);
  }
  return true;
}

}  // namespace pt00emu
