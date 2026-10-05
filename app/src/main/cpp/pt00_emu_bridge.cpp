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
void pt00_emu_set_intd(const int* src, unsigned count);
void pt00_emu_get_intd(int* dst, unsigned count);
}

namespace {

constexpr int kIntDCount = 2000;

bool g_tried = false;
bool g_ready = false;
std::vector<int> g_intd(kIntDCount);

int GetD(RLMachine& machine, int index) {
  return machine.GetIntValue(
      libreallive::IntMemRef(libreallive::INTD_LOCATION, index));
}

void SetD(RLMachine& machine, int index, int value) {
  machine.SetIntValue(libreallive::IntMemRef(libreallive::INTD_LOCATION, index),
                      value);
}

}  // namespace

namespace pt00emu {

bool Available() { return g_ready; }

void Reset() {
  g_tried = false;
  g_ready = false;
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
    std::cerr << "[pt00] 兼容层就绪：执行器直接跑原版 PT00.dll（" << bytes.size()
              << " 字节）" << std::endl;
  }
  if (!g_ready) return false;

  // 真机诊断：前 40 次调用打出来，用来确认脚本确实走到了 PT00，
  // 以及它按什么顺序驱动 func（日志走 rlvm-stderr）。
  static int logged = 0;
  if (logged < 40) {
    ++logged;
    std::cerr << "[pt00] CallDLL func=" << func << " a=(" << a1 << "," << a2
              << "," << a3 << "," << a4 << ")" << std::endl;
  }

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
      std::cerr << os.str() << std::endl;
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
  // 跑完再把改动写回引擎（只写真正变了的槽，省点 SetIntValue 的开销）。
  for (int i = 0; i < kIntDCount; ++i) g_intd[i] = GetD(machine, i);
  pt00_emu_set_intd(g_intd.data(), kIntDCount);
  pt00_emu_call(func, a1, a2, a3, a4);
  pt00_emu_get_intd(g_intd.data(), kIntDCount);
  for (int i = 0; i < kIntDCount; ++i) {
    if (g_intd[i] != GetD(machine, i)) SetD(machine, i, g_intd[i]);
  }
  return true;
}

}  // namespace pt00emu
