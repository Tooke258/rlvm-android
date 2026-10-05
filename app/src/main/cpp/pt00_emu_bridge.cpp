#include "pt00_emu_bridge.h"

#include <iostream>
#include <string>
#include <vector>

#include "libreallive/intmemref.h"
#include "machine/rlmachine.h"
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

  // intD 是 DLL 与脚本唯一的交换区：进来先把引擎的值灌给执行器，
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
