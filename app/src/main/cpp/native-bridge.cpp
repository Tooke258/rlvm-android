// JNI 桥接层。
//
// task.md 硬性约束：所有 native 方法必须用 RegisterNatives 显式注册，不使用按符号名的动态查找。
// 因此本文件不声明 Java_org_rlvm_android_* 形式的导出函数，而是统一在 JNI_OnLoad 中绑定。

#include <jni.h>

#include <android/log.h>

#include <exception>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

#include <unistd.h>

#include <fcntl.h>
#include <sys/stat.h>

// Boost 1.92 起 path.hpp / operations.hpp 不再传递包含 directory.hpp，必须显式引入。
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libreallive/archive.h"
#include "libreallive/bytecode.h"
#include "libreallive/gameexe.h"
#include "libreallive/intmemref.h"
#include "long_operations/button_object_select_long_operation.h"
#include "machine/rloperation/references.h"
#include "machine/rloperation/rlop_store.h"
#include "android/android_system.h"
#include "android/android_graphics.h"
#include "android/audio_engine.h"
#include "android/game_file_system.h"
#include "android/jni_saf_backend.h"
#include "android/log_redirect.h"
#include "android/mov_probe.h"
#include "android/mov_player.h"
#include "android/app_log.h"
#include "android/text_encoding.h"
#include "android/font_engine.h"
#include "android/saf_file_system.h"
#include "machine/game_hacks.h"
#include "machine/dump_scenario.h"
#include "machine/long_operation.h"
#include "machine/rlmachine.h"
#include "machine/rlmodule.h"
#include "machine/rloperation.h"
#include "machine/serialization.h"
#include "modules/modules.h"
#include "systems/base/event_system.h"
#include "systems/base/frame_counter.h"
#include "systems/base/graphics_system.h"
#include "systems/base/graphics_object.h"
#include "systems/base/parent_graphics_object_data.h"
#include "systems/base/little_busters_pt00dll.h"
#include "pt00_emu_bridge.h"
#include "utilities/file.h"
#include "utilities/string_utilities.h"
#include "utf8cpp/utf8.h"

namespace {

constexpr char kLogTag[] = "rlvm-native";

// ---------------------------------------------------------------------------
// 帧呈现缓冲
//
// 引擎在自己的线程上合成帧，GL 线程按自己的节奏取走最新的一帧。
// 这里保存一份拷贝而不是直接暴露 AndroidGraphicsSystem 的帧缓冲：
// 引擎实例的生命周期（T2.4）尚未定型，拷贝可以让两边解耦。
// ---------------------------------------------------------------------------
std::mutex g_frame_mutex;
std::vector<uint32_t> g_frame_pixels;
int g_frame_width = 0;
int g_frame_height = 0;
unsigned int g_frame_serial = 0;
// 逐帧日志的缺省值：**关闭**（0）。开着时每帧要扫 48 万像素统计非黑像素数，
// 在真机上是最没意义的一笔开销；排查渲染问题时再用 frame_log_every=N 打开。
int g_frame_log_every = 0;
// 合成成本日志（diag 键 blit_cost=1 打开）。见 RunEngineOn 里 progress 行旁边的说明。
bool g_blit_cost_log = false;

// ---------------------------------------------------------------------------
// 设备侧诊断开关
//
// 为什么需要：真机迭代一轮要重新构建 + 安装（数分钟），而「首屏为什么是黑的」
// 这类问题需要反复调整观察参数（跑多久、要不要逐条指令追踪）。
// 因此把参数放进一个纯文本文件，用 adb push 改一行就能换一次实验，
// 不必重新编译。文件由 Kotlin 侧告知路径（本机外部文件目录）。
//
// 文件格式：每行 `key=value`，`#` 开头为注释。缺省值即原行为。
//   trace=1              逐条指令追踪（上游 set_tracing_on，输出走 stderr）
//   time_budget_ms=8000  单次运行的执行时间片
//   max_instructions=N   指令条数上限
//   frame_log_every=60   每 N 帧才打一条帧日志（默认 1，即每帧）
// ---------------------------------------------------------------------------
std::mutex g_diag_mutex;
std::string g_diag_dir;

// 「停止引擎」请求。由 UI 线程置位，引擎线程在每轮循环开头检查。
// 应用默认持续运行（见 DiagOptions::time_budget_ms 的 0 = 不限时），
// 因此必须有一个从外部喊停的通道。
std::atomic<bool> g_stop_requested{false};

// 「挂起引擎」请求：黑屏 / 应用进后台时由 UI 线程置位，引擎线程在每轮循环开头检查。
//
// 之前没有这条通道：屏幕熄灭后 GL 线程被 onPause 停住，引擎线程却照样按 10ms 时间片
// 推进字节码、音频回调照样出声——用户看到的就是「黑屏之后游戏还在跑」。挂起期间既
// 不推进指令也不合成新帧，音频同时被打成静音并停掉数据回调；置回 false 后从原位置继续。
std::atomic<bool> g_engine_suspended{false};
// 挂起期间是否已经落过 global memory（每挂起一次只落一次）。
std::atomic<bool> g_suspend_flushed{false};
// 渲染树导出请求（v0.2.4）：面板按钮置位，引擎线程在循环里导出一次。
// 为什么不在 UI 线程直接导出：只有引擎线程碰 graphics，跨线程会撕裂。
std::atomic<bool> g_dump_tree_request{false};

// 对象活体时间线（diag: wipe_log=1）用的上一帧摘要。只在内容变化时打印一行。
std::string g_last_obj_digest;

// 输入取证（diag: input_trace=1）的上一行内容，只在变化时打印。
std::string g_last_input_line;

// 文本容器（例如合并了汉化的 SEEN 归档）在游戏目录树内的相对路径。
// 空 = 用游戏目录里的 Seen.txt。UI 线程写入、引擎线程读取，所以加锁。
std::mutex g_text_container_mutex;
std::string g_text_container_path;

// ---------------------------------------------------------------------------
// 内存探针（诊断用）
//
// 背景：真机上出现过「引擎解析到一半内存暴涨，被系统 lowmemorykiller SIGKILL，
// 连带把系统文件管理器一起杀掉」。为了定位是哪一步在涨内存，这里读 /proc/self/status
// 的 VmRSS 并打日志；同时给一个自身上限，超过就主动停手，避免再把整机拖下水。
// ---------------------------------------------------------------------------
constexpr long kMemoryGuardKb = 1200 * 1024;  // 1.2 GB

long CurrentRssKb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 6, "VmRSS:") != 0) continue;
    try {
      return std::stol(line.substr(6));
    } catch (...) {
      return -1;
    }
  }
  return -1;
}

void LogMemory(const char* phase) {
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "mem[%s] rss=%ld kB", phase,
                      CurrentRssKb());
}

// 是否已有一台引擎在跑。默认不限时运行后，重复点「运行」很容易起第二台，
// 两台引擎共用同一个 AudioEngine 与帧缓冲会互相踩踏，所以直接拒绝并存。
std::atomic<bool> g_run_active{false};

/**
 * 规整"游戏文件标识"。
 *
 * SAF 模式下我们把 gameexe 的 __GAMEPATH 设成了 "saf:/"，而引擎会用它拼出
 * 存档/全局数据的完整路径（例如 "saf://SAVEDATA/save001.sav"）。这个前缀对
 * SAF 后端没有意义——它要的是相对路径（"SAVEDATA/save001.sav"）。
 * 不剥掉就会去"创建名为 saf: 的目录"从而失败，表现正是"存档/Config 不保留"。
 */
std::string NormalizeGameFileId(const char* file_id) {
  std::string id(file_id);
  const std::string prefix = "saf:";
  // 只有确实带 saf: 前缀时才剥掉它**以及**紧随其后的斜杠；普通绝对路径
  //（例如 HOME 下的存档路径 /storage/...）必须原样保留开头的斜杠，
  // 否则会被当成 SAF 相对路径——上一版就是这样把存档写错了地方。
  if (id.compare(0, prefix.size(), prefix) == 0) {
    id.erase(0, prefix.size());
    while (!id.empty() && id[0] == '/') id.erase(0, 1);
  }
  return id;
}

/**
 * 平台钩子实现：把「游戏文件标识」换成只读 fd。
 *
 * 语音归档（KOE/NWK/OVK/koepac）沿用上游的 fopen/ifstream 按路径读文件，
 * 这在 SAF 下必然失败（没有真实路径）。上游在 utilities/file.h 里留了钩子，
 * 这里实现它：普通路径后端返回真实 fd，SAF 后端由 Kotlin 侧的 ContentResolver
 * 打开同一个文档，两者对调用方没有区别。
 */
int OpenGameFileFdHookImpl(const char* file_id) {
  if (file_id == nullptr) return -1;
  const std::string id = NormalizeGameFileId(file_id);
  // 绝对路径（例如 HOME 指向的应用目录）：直接走 posix，不属于 SAF 树。
  if (!id.empty() && id[0] == '/') return ::open(id.c_str(), O_RDONLY);
  std::shared_ptr<rlvm_android::GameFileSystem> files =
      rlvm_android::GetGameFileSystem();
  if (!files) return -1;
  return files->OpenFd(id);
}

/** 写入变体：存档与全局数据（Config）经这里落盘，SAF 下由 Kotlin 建文档。 */
int OpenGameFileWriteFdHookImpl(const char* file_id) {
  if (file_id == nullptr) return -1;
  std::shared_ptr<rlvm_android::GameFileSystem> files =
      rlvm_android::GetGameFileSystem();
  if (!files) return -1;
  const std::string id = NormalizeGameFileId(file_id);
  // 绝对路径：存档/全局数据可能落在 HOME（应用外部目录）下，那里是真实路径，
  // 不需要也不能走 SAF。父目录不存在时先创建。
  if (!id.empty() && id[0] == '/') {
    const size_t slash = id.find_last_of('/');
    if (slash != std::string::npos) ::mkdir(id.substr(0, slash).c_str(), 0755);
    const int posix_fd = ::open(id.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    __android_log_print(posix_fd >= 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kLogTag,
                        "write open (posix): %s (fd=%d)", id.c_str(), posix_fd);
    return posix_fd;
  }
  const int fd = files->OpenWriteFd(id);
  if (fd >= 0) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "write open: %s (fd=%d)", id.c_str(), fd);
  } else {
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "write open failed: %s", id.c_str());
  }
  return fd;
}

// 当前正在运行的 AndroidSystem。UI 线程的触摸事件需要它才能找到事件系统；
// 引擎停止后清空，避免触到已析构的对象。
std::atomic<AndroidSystem*> g_current_system{nullptr};

// 一次触摸等价于哪个鼠标键（由 diag 的 touch_button 决定，运行期只读）。
std::atomic<int> g_touch_buttons{1};

// 触摸事件的动作码，必须与 NativeBridge.touchEvent 的调用方一致。
constexpr int kTouchDown = 0;
constexpr int kTouchMove = 1;
constexpr int kTouchUp = 2;

/** 作用域内的「唯一运行者」守卫；未取得时 acquired() 为 false。 */
class RunGuard {
 public:
  RunGuard() : acquired_(!g_run_active.exchange(true)) {}
  ~RunGuard() {
    if (acquired_) g_run_active.store(false);
  }
  RunGuard(const RunGuard&) = delete;
  RunGuard& operator=(const RunGuard&) = delete;
  bool acquired() const { return acquired_; }

 private:
  bool acquired_;
};

/** 让 UI 线程能找到当前引擎的事件系统；离开作用域即断开。 */
class CurrentSystemGuard {
 public:
  explicit CurrentSystemGuard(AndroidSystem* system) {
    g_current_system.store(system);
  }
  ~CurrentSystemGuard() { g_current_system.store(nullptr); }
  CurrentSystemGuard(const CurrentSystemGuard&) = delete;
  CurrentSystemGuard& operator=(const CurrentSystemGuard&) = delete;
};

struct DiagOptions {
  bool trace = false;
  // 诊断：dump_scenes=7300,7450 → 引擎启动时把这几幕的脚本反汇编打到日志
  // （用来读小游戏那段循环在等什么；不需要真的走到小游戏）
  std::string dump_scenes;
  // 取证：pt00_trace_ctx=1 → 记录 PT00 执行器对 ctx 块/低地址的读写，
  // 看 DLL 是不是在找别的引擎数组（intG 那条通道）。
  bool pt00_trace_ctx = false;
  // 试验：pt00_tick31=1 → 每帧首替脚本补调一次 CallDLL(0,31)（原引擎的小游戏驱动）
  bool pt00_tick31 = false;
  // 小游戏逐调用日志（默认关；见 pt00_emu_bridge.h 的 SetVerbose 注释）。
  bool pt00_verbose = false;
  // 执行器观察点：pt00_watch=10004932（十六进制 eip）时，每次 CallDLL 首次命中该
  // 地址就打印寄存器 + [edi-0x10] 结构体窗口。死循环里的寄存器被 add 推歪之后
  // 看不出入口现场，这是唯一能拿回「循环入口 begin/end」的手段。
  unsigned pt00_watch = 0;
  bool dump_graphics = false;
  bool audio_selftest = false;
  // 合成统计（逐像素累加）默认关闭，避免拖慢渲染。
  bool blit_stats = false;
  // blit_cost=1：每 60 帧打一行合成成本（调用数 / 写入像素 / 总耗时 / 最慢一次及其矩形）。
  // 用来回答「卡在哪一层」——探针计数一直在维护，但此前没有消费者，从没被量过。
  bool blit_cost = false;
  // wipe_log=1：打印对象「晋升/擦除」明细 + 195..255 号对象的活体时间线。
  // 用于定位「小游戏地图（objFg #201）在运镜结束后消失」。默认关。
  bool wipe_log = false;
  // input_trace=1：每帧（变化才打）记录「pad 按住了什么 / 引擎光标在哪 /
  // intD[95..115] 的值」。用来回答「按键到底有没有走到引擎侧、以及引擎会不会
  // 把输入写进 intD」——小游戏方向键不通的第一步排查。默认关。
  bool input_trace = false;
  // intd_poke：诊断用的"往 intD 写值"扫描器。按住任意方向键时生效，
  // 每 intd_poke_hold 秒换下一个槽位，并把当前正在写的 (槽位,值) 打进日志。
  // 用途：直接验证"引擎把方向写进哪个 intD 槽、写什么值能让角色动"。
  // 默认关；intd_poke_lo/hi 默认 100..110，值默认 1。
  bool intd_poke = false;
  int intd_poke_lo = 100;
  int intd_poke_hi = 110;
  int intd_poke_value = 1;
  int intd_poke_hold_ms = 6000;
  // intd_poke_free=1：不要求按住方向键，扫描器一直生效。
  // 用来"读方向编码"：轮流往 intD[103..107] 写 1，用户只要看角色往哪动。
  bool intd_poke_free = false;
  // intd_dir_poke=1：把 pad 的四个方向键**直接映射**成 intD 标志位——
  // 按住某方向时写 intD[intd_dir_xxx] = 1，松开时写回 0（只写一次，不持续覆盖）。
  // 这既是"读编码"的手段（看角色往哪动），也是将来真正输入 op 的形态。
  bool intd_dir_poke = true;
  int intd_dir_up = 104;
  int intd_dir_down = 106;
  int intd_dir_left = 107;
  int intd_dir_right = 105;
  // intd_hit_poke=1：把「击打」（鼠标左键）直接写进 intD[intd_hit_slot]（默认 101）。
  // 脚本判挥棒用 intD[101] == 1；实测即使用户按击打，intD[101] 也一直是 0，
  // 于是没有挥棒 → 没有投球循环 → 球和"猫"都不会出现。
  bool intd_hit_poke = true;
  int intd_hit_slot = 101;
  // intd_right_poke=1（默认开）：右键（= 长按手势松开那一帧）→ intD[102] = 2。
  // 原生引擎每帧把鼠标/键盘写进 intD[100..108]（101=左键、102=右键），脚本
  // SEEN7800 用 `intD[102] == 2` 开**暂停菜单**（菜单本体 = 221 号对象
  // PT_RMENU_BG00/BTN00/BTN01，二级确认 = 222 号 PT_ENDWIN_*）。
  // 只发"松开脉冲"而不整个镜像按钮状态机（1=按住 / 2=抬起），避免把
  // `intD[101] == 1` 的挥棒判定带偏。
  bool intd_right_poke = true;
  int intd_right_slot = 102;
  int intd_right_value = 2;
  int intd_right_pulse_frames = 2;
  // op_trace=a,b,c：只追踪「名字含任一子串」的指令（逗号分隔白名单）。
  // 非空即等价于打开 trace，但只打印白名单里的指令——用来在 46MB 全量 trace
  // 里只盯会改对象状态的那十来个 op。默认空 = 不过滤（行为同原来）。
  std::string op_trace;
  // case_trace=1：把 goto_case / goto_on 的求值与命中打一行
  // `[case] SEENxxxx Lnnn value=? n=? [i](case)… -> hit/default/none`
  // 用途：暂停菜单里 `goto_case(intA[i])` 选图案号，确认为什么全部落在 0 号脸。
  bool case_trace = false;
  // patno_trace=1：`objPattNo` 写入取证（`[patno] SEENxxxx Lnnn parent=? child=?
  // set=? now=?`），用于定位「暂停菜单图标全是 0 号脸」。
  bool patno_trace = false;
  // 诊断：blit_fast=0 关闭 D-022 的 blit 优化（内容包围盒裁剪 + 不透明 memcpy）。
  bool blit_fast = true;
  // 合帧闸门默认**关闭**（每轮无条件合帧）——开启会让过场出现整屏黑闪，
  // 机理见 android_system.cpp 里 g_dirty_gate 的注释。写 dirty_gate=1 可打开做 A/B。
  bool dirty_gate = false;
  // v0.2.3 影片探针：mov_probe=MOV/op00.mpg 时，在引擎启动时跑一次
  // 「MPEG-PS 解复用 + AMediaCodec(video/mpeg2)」的最小闭环，结果写进报告。
  std::string mov_probe;
  // M3a 对照：用平台 AMediaExtractor 解同一个 .mpg（值是设备上的路径，
  // 若只给文件名则视为诊断目录下的文件）。见 android/mov_probe.h。
  std::string mov_probe_path;
  // 诊断用：指定解码器名（空 = 按 video/mpeg2 自动挑）。例如
  // `mov_codec=c2.android.mpeg2.decoder` 换软解，跟 MTK 硬解做 A/B。
  std::string mov_codec;
  // 诊断用：是否给解码器 csd-0/宽高提示（默认给）。mov_csd=0 时不给，
  // 让硬解自己从码流认 MPEG-1（部分硬解给了 MPEG-2 风格 csd 会按错语法解）。
  // 0=不给；1=给；2=两种都跑（A/B，一次引擎启动出两组结果）
  int mov_csd = 1;
  // v0.2.3 / M3b 影片上屏自测：引擎启动时直接起播这个影片（值是 MOV 下的文件名，
  // 或完整的游戏文件标识），用于在没有脚本触发点的游戏里肉眼验证上屏。
  std::string mov_test;
  int mov_test_ms = 0;  // > 0 时播这么久就停
  bool mov_test_loop = false;  // 自测也用循环播放（验 movLoop 的通路）
  // D-033 自检：跑一次「GBK→CP932」编码链的逐步诊断（进 report 与应用日志）。
  bool enc_selftest = false;
  // v0.2.4 自检：字体布局宽度 vs 渲染宽度（英文左右分布不齐）。
  bool font_selftest = false;
  bool font_probe = false;  // 每次字形渲染一行日志（查图标/裁切问题）
  // 汉化用：把 SEEN.TXT 每个场景的文本串按顺序导出（见 docs/LOCALIZATION.md）。
  bool export_jp_text = false;
  // 一次触摸等价于哪个鼠标键（位掩码：1=左键 2=右键 3=两者）。
  // 不同 RealLive 作品的脚本约定不一致，因此做成设备侧可调。
  int touch_button = 1;
  // M2 小游戏通路：1 = 不注册上游的「跳过 LB 棒球小游戏」hack，让脚本真正
  // 进入 SEEN7030，同时打开 PT00 的调用日志（见 docs/MINIGAME-PLAN.md）。
  // 上游 machine/game_hacks.cc 属受保护目录，所以开关放在平台层。
  bool lb_minigame = false;
  // 0 表示不限时：应用要能一直停在标题/正文上，BGM 才不会「响一下就没了」。
  // 自动化测试需要在报告里拿到结果时，用 diag 文件设一个有限值。
  int time_budget_ms = 0;
  int max_instructions = 0;  // 0 表示沿用调用方传入的值
  // 0 = 不打逐帧日志（缺省，见上面 g_frame_log_every 的说明）
  int frame_log_every = 0;
};

/** 读取并解析诊断文件；文件不存在时返回缺省值。 */
DiagOptions LoadDiagOptions() {
  DiagOptions options;

  std::string dir;
  {
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    dir = g_diag_dir;
  }
  if (dir.empty()) return options;

  std::ifstream stream(dir + "/rlvm-diag.txt");
  if (!stream) return options;

  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::string::size_type equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    const int number = std::atoi(value.c_str());
    if (key == "trace") {
      options.trace = (number != 0);
    } else if (key == "dump_graphics") {
      options.dump_graphics = (number != 0);
    } else if (key == "audio_selftest") {
      options.audio_selftest = (number != 0);
    } else if (key == "blit_stats") {
      options.blit_stats = (number != 0);
    } else if (key == "blit_cost") {
      options.blit_cost = (number != 0);
    } else if (key == "wipe_log") {
      options.wipe_log = (number != 0);
    } else if (key == "op_trace") {
      options.op_trace = value;
    } else if (key == "case_trace") {
      options.case_trace = (number != 0);
    } else if (key == "patno_trace") {
      options.patno_trace = (number != 0);
    } else if (key == "input_trace") {
      options.input_trace = (number != 0);
    } else if (key == "intd_poke") {
      options.intd_poke = (number != 0);
    } else if (key == "intd_poke_lo") {
      options.intd_poke_lo = number;
    } else if (key == "intd_poke_hi") {
      options.intd_poke_hi = number;
    } else if (key == "intd_poke_value") {
      options.intd_poke_value = number;
    } else if (key == "intd_poke_hold_ms") {
      if (number > 0) options.intd_poke_hold_ms = number;
    } else if (key == "intd_poke_free") {
      options.intd_poke_free = (number != 0);
    } else if (key == "intd_dir_poke") {
      options.intd_dir_poke = (number != 0);
    } else if (key == "intd_dir_up") {
      options.intd_dir_up = number;
    } else if (key == "intd_dir_down") {
      options.intd_dir_down = number;
    } else if (key == "intd_dir_left") {
      options.intd_dir_left = number;
    } else if (key == "intd_dir_right") {
      options.intd_dir_right = number;
    } else if (key == "intd_hit_poke") {
      options.intd_hit_poke = (number != 0);
    } else if (key == "intd_hit_slot") {
      options.intd_hit_slot = number;
    } else if (key == "intd_right_poke") {
      options.intd_right_poke = (number != 0);
    } else if (key == "intd_right_slot") {
      options.intd_right_slot = number;
    } else if (key == "intd_right_value") {
      options.intd_right_value = number;
    } else if (key == "intd_right_pulse_frames") {
      if (number > 0) options.intd_right_pulse_frames = number;
    } else if (key == "blit_fast") {
      options.blit_fast = (number != 0);
    } else if (key == "dirty_gate") {
      options.dirty_gate = (number != 0);
    } else if (key == "mov_probe") {
      options.mov_probe = value;
    } else if (key == "dump_scenes") {
      options.dump_scenes = value;
    } else if (key == "pt00_trace_ctx") {
      options.pt00_trace_ctx = (number != 0);
    } else if (key == "pt00_tick31") {
      options.pt00_tick31 = (number != 0);
    } else if (key == "pt00_verbose") {
      options.pt00_verbose = (number != 0);
    } else if (key == "pt00_watch") {
      options.pt00_watch =
          static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16));
    } else if (key == "mov_probe_path") {
      options.mov_probe_path = value;  // 值是设备上的路径或诊断目录下的文件名
    } else if (key == "mov_codec") {
      options.mov_codec = value;  // 解码器名；空 = 按 video/mpeg2 自动挑
    } else if (key == "mov_csd") {
      options.mov_csd = number;  // 0=不给 csd；1=给；2=给/不给都跑一遍
    } else if (key == "mov_test") {
      options.mov_test = value;  // 例：mov_test=OP00 或 mov_test=MOV/OP00.mpg
    } else if (key == "mov_test_ms") {
      if (number > 0) options.mov_test_ms = number;
    } else if (key == "enc_selftest") {
      options.enc_selftest = (number != 0);
    } else if (key == "mov_test_loop") {
      options.mov_test_loop = (number != 0);
    } else if (key == "font_selftest") {
      options.font_selftest = (number != 0);
    } else if (key == "font_probe") {
      options.font_probe = (number != 0);
    } else if (key == "export_jp_text") {
      options.export_jp_text = (number != 0);
    } else if (key == "time_budget_ms") {
      if (number > 0) options.time_budget_ms = number;
    } else if (key == "max_instructions") {
      if (number > 0) options.max_instructions = number;
    } else if (key == "frame_log_every") {
      if (number > 0) options.frame_log_every = number;
    } else if (key == "touch_button") {
      if (number > 0) options.touch_button = number;
    } else if (key == "lb_minigame") {
      options.lb_minigame = (number != 0);
    }
  }
  return options;
}

/** 把图形系统当前的帧缓冲拷进呈现缓冲。 */
/** 生成 "WINDOW.032.ATTR_MOD" 这种三位零填充的 Gameexe 键。 */
std::string WindowGameexeKey(int index, const std::string& suffix) {
  std::string key = "WINDOW.";
  key += static_cast<char>('0' + (index / 100) % 10);
  key += static_cast<char>('0' + (index / 10) % 10);
  key += static_cast<char>('0' + index % 10);
  key += suffix;
  return key;
}

/** 取 Gameexe.ini 文本里 "#WINDOW.000.<suffix>=<value>" 的 value（找不到返回空）。 */
std::string FindWindow000Value(const std::string& text,
                               const std::string& suffix) {
  const std::string needle = "#WINDOW.000." + suffix + "=";
  const std::string::size_type pos = text.find(needle);
  if (pos == std::string::npos) return std::string();
  const std::string::size_type begin = pos + needle.size();
  std::string::size_type end = text.find_first_of("\r\n", begin);
  if (end == std::string::npos) end = text.size();
  const std::string raw = text.substr(begin, end - begin);
  const std::string::size_type a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  const std::string::size_type b = raw.find_last_not_of(" \t");
  return raw.substr(a, b - a + 1);
}

/**
 * 汉化版 GAMEEXE.INI 只给"常规"窗口（000..010、012..014）写全属性；小游戏用的
 * 特例窗口（020/021/031/032）只有 POS、MOJI_xxx、WAKU_SETNO，缺 ATTR_MOD、
 * KEYCUR_MOD 等键。真实 RealLive 引擎对缺失键按默认值处理（ATTR_MOD 缺省 = 0，
 * 即"改用全局 #WINDOW_ATTR"，RLVM 自己的代码里就有这一支），但 RLVM 的
 * GameexeInterpretObject 会抛 Unknown Gameexe key —— TextWindow 构造失败，
 * 而构造不出来就永远不进缓存，于是每帧重试、每帧抛异常：阻塞型 op（Msg 17
 * «pause»）永远完不成。
 *
 * 真机现象：小游戏结束后回到 SEEN515 的第一句对话，画面冻结、音乐正常、引擎
 * 55Hz 空转，stdout 反复刷 "Unknown Gameexe key 'WINDOW.032.XXX'"。
 *
 * 这里按"用窗口 000 的同名值兜底"补齐：只给**数据里真实存在**的窗口补，且只补
 * 缺失的键（key 已存在时 parseLine 的 insert 不会覆盖）。
 */
void AddMissingWindowDefaults(Gameexe& gameexe, const std::string& gameexe_text) {
  /* 只补 TextWindow 构造函数里**没有默认值**的那些键。像 WAKU_SETNO / MOJI_SIZE /
   * LUBY_SIZE / INDENT_USE / R_COMMAND_MOD / NAME_MOD 这些，RLVM 用的是 `.ToInt(默认)`
   * ——它们本来就不会抛，改成"照抄窗口 000"反而会改掉原有行为（上一版就是这么把
   * 小游戏带崩的）。 */
  static const char* const kSuffixes[] = {
      "ATTR_MOD", "ATTR", "KEYCUR_MOD", "MOJI_CNT", "MOJI_REP", "MOJI_POS", "POS",
      /* 名字框（名牌）那一族：TextWindow 只在「NAME_MOD==1 且 NAME_WAKU_SETNO 存在」
       * 时才创建 namebox_waku_，但 Render() 只要有说话人名就会去解引用它
       * （text_window.cc:341/449 在 443 的判空之前）—— 数据缺这些键时真机会直接
       * 空指针崩溃（tombstone #00 TextWindow::GetNameboxWakuRect）。缺哪个补哪个，
       * 值照抄窗口 000。 */
      "NAME_MOD", "NAME_WAKU_SETNO", "NAME_MOJI_REP", "NAME_MOJI_POS",
      "NAME_POS", "NAME_MOJI_SIZE"};

  bool seen[100] = {false};
  for (std::string::size_type pos = 0;
       (pos = gameexe_text.find("#WINDOW.", pos)) != std::string::npos;
       pos += 8) {
    if (pos + 11 >= gameexe_text.size()) continue;
    const char* d = gameexe_text.data() + pos + 8;
    if (d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9' ||
        d[2] < '0' || d[2] > '9' || d[3] != '.')
      continue;
    const int n = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
    if (n >= 0 && n < 100) seen[n] = true;
  }

  int added = 0;
  for (int i = 0; i < 100; ++i) {
    if (!seen[i]) continue;
    for (const char* suffix : kSuffixes) {
      const std::string key = WindowGameexeKey(i, std::string(".") + suffix);
      if (gameexe.Exists(key)) continue;
      std::string value = FindWindow000Value(gameexe_text, suffix);
      if (value.empty()) value = "0";
      // 用 parseLine 注入（与文件解析同一路径：多值/带引号的值都能正确落地）。
      gameexe.parseLine("#" + key + "=" + value);
      ++added;
    }
  }
  if (added > 0) {
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Gameexe: 补 %d 个缺失的窗口属性键（窗口 000 同名值兜底）",
                        added);
  }
}

void CaptureFrame(AndroidGraphicsSystem& graphics) {
  std::shared_ptr<AndroidSurface> frame = graphics.frame_buffer();
  if (!frame) return;

  const Size size = frame->GetSize();
  const size_t count = static_cast<size_t>(size.width()) *
                       static_cast<size_t>(size.height());
  if (count == 0) return;

  std::lock_guard<std::mutex> lock(g_frame_mutex);
  g_frame_pixels.assign(frame->pixels(), frame->pixels() + count);
  g_frame_width = size.width();
  g_frame_height = size.height();
  ++g_frame_serial;

  // 非黑像素数：用于区分「引擎产出的帧本身是空的」与「呈现环节没显示出来」。
  // 帧日志可以按间隔抽样：逐帧输出在长时间运行时会淹没真正重要的诊断信息。
  // 0 = 完全不打（缺省）；1 = 每帧打；N>1 = 每 N 帧打一条。
  const bool log_this_frame =
      g_frame_log_every == 1 ||
      (g_frame_log_every > 1 &&
       (g_frame_serial % static_cast<unsigned int>(g_frame_log_every)) == 1u);
  if (log_this_frame) {
    // 注意：非黑像素统计要扫全屏 48 万像素，只在真的要打印时才做——
    // 之前它每帧都跑，是这个渲染管线里最没意义的一笔开销。
    size_t non_black = 0;
    for (size_t i = 0; i < count; ++i) {
      if ((g_frame_pixels[i] & 0x00FFFFFFu) != 0) ++non_black;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "frame %dx%d serial=%u nonblack=%zu/%zu", g_frame_width,
                        g_frame_height, g_frame_serial, non_black, count);
  }
}

/** 把 Java 字符串转成 UTF-8 的 std::string。 */
std::string JStringToUtf8(JNIEnv* env, jstring value) {
  if (value == nullptr) return std::string();
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result = (chars != nullptr) ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
  return result;
}

/**
 * native -> Java 的字符串出口。**所有**返回给 Kotlin 的字符串都必须走这里。
 *
 * JNI 的 NewStringUTF 要求 Modified UTF-8；而游戏数据里的字符串往往是 Shift-JIS
 *（例如 Kud Wafter 的 #REGNAME = "KEY\クドわふたー"，首字节 0x83）。
 * 直接把这种字节交给 JNI 会触发
 *   "JNI DETECTED ERROR IN APPLICATION: input is not valid Modified UTF-8"
 * 并让 ART 直接 abort 整个进程——实测表现就是「跑一下应用就被杀」。
 */
jstring NewSafeJavaString(JNIEnv* env, const std::string& text) {
  if (utf8::is_valid(text.begin(), text.end())) {
    return env->NewStringUTF(text.c_str());
  }

  // 不是合法 UTF-8：按 CP932（RealLive 的原生编码）转一次再试。
  const std::string converted = cp932toUTF8(text, 0);
  if (utf8::is_valid(converted.begin(), converted.end())) {
    return env->NewStringUTF(converted.c_str());
  }

  // 兜底：替换掉非 ASCII 字节，保证一定能构造出 Java 字符串而不是让进程 abort。
  std::string sanitized;
  sanitized.reserve(text.size());
  for (unsigned char c : text) {
    sanitized.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return env->NewStringUTF(sanitized.c_str());
}

/**
 * 逐行输出报告。
 *
 * 必须逐行：logcat 对单条消息有长度上限（约 1000 字符），把几千字的报告塞进一次
 * __android_log_print 会让后半部分静默消失——实测中这会被误判成「运行卡住了」。
 */
void LogReport(const char* tag, const char* title, const std::string& report) {
  __android_log_print(ANDROID_LOG_INFO, tag, "%s:", title);
  std::string::size_type begin = 0;
  while (begin <= report.size()) {
    std::string::size_type end = report.find('\n', begin);
    if (end == std::string::npos) end = report.size();
    if (end > begin) {
      __android_log_print(ANDROID_LOG_INFO, tag, "%.*s", static_cast<int>(end - begin),
                          report.c_str() + begin);
    }
    begin = end + 1;
  }
}

/**
 * 供日志与报告显示用：把可能是 Shift-JIS 的游戏字符串转成 UTF-8。
 * 只在字符串不是合法 UTF-8 时才转换，因此 ASCII 与我们自己的文本原样保留。
 */
std::string ToDisplayUtf8(const std::string& text) {
  if (utf8::is_valid(text.begin(), text.end())) return text;
  const std::string converted = cp932toUTF8(text, 0);
  if (utf8::is_valid(converted.begin(), converted.end())) return converted;
  return std::string();
}

/** 返回版本串，用于验证 Kotlin <-> JNI 链路（T1.4 的最小验证）。 */
jstring VersionString(JNIEnv* env, jobject /*thiz*/) {
  return env->NewStringUTF("rlvm-android native bridge 0.1.0 (T0.2 skeleton)");
}

/** 返回指针位宽，用于确认实际加载的 ABI 与预期一致。 */
jint ProbeAbi(JNIEnv* /*env*/, jobject /*thiz*/) {
#if defined(__aarch64__)
  return 64;
#elif defined(__arm__)
  return 32;
#else
  return 0;
#endif
}

/**
 * 真正的引擎探针：在给定的目录里解析 Gameexe.ini 并打开 SEEN 归档。
 *
 * 这是 Android 上第一次执行 RLVM 的实际逻辑——会走通 boost::filesystem 访问、
 * libreallive 的 Gameexe 解析、以及 Archive + Mapping 的场景归档读取。
 * 返回值是供人阅读的多行报告；异常被捕获后也会写进报告，避免 native 崩溃。
 */
jstring ProbeGameDir(JNIEnv* env, jobject /*thiz*/, jstring jdir) {
  namespace fs = boost::filesystem;
  std::string report;

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    report += "dir = " + dir + "\n";

    const fs::path root(dir);
    if (!fs::exists(root)) {
      report += "ERROR: directory does not exist\n";
      return NewSafeJavaString(env, report);
    }
    if (!fs::is_directory(root)) {
      report += "ERROR: not a directory\n";
      return NewSafeJavaString(env, report);
    }

    report += "files:\n";
    for (fs::directory_iterator it(root); it != fs::directory_iterator(); ++it) {
      const fs::path& p = it->path();
      // 目录条目上调用 file_size 在 Android 上会抛「Function not implemented」，
      // 因此只对普通文件取大小。
      if (fs::is_directory(p)) {
        report += "  " + p.filename().string() + "/  <dir>\n";
      } else {
        report += "  " + p.filename().string() + "  " +
                  std::to_string(fs::file_size(p)) + " bytes\n";
      }
    }

    // --- Gameexe.ini ---
    // 上游的 CorrectPathCase 负责大小写纠正：RealLive 游戏来自 Windows/FAT，
    // 文件名大小写常不规整，而 Android 文件系统区分大小写。
    const fs::path gameexe_path = CorrectPathCase(root / "Gameexe.ini");
    if (!gameexe_path.empty()) {
      // 注意：Gameexe 位于全局命名空间，只有 Archive / Scenario 在 libreallive 里。
      Gameexe gameexe(gameexe_path);
      const std::string caption = gameexe("CAPTION").ToString("");
      report += "Gameexe: parsed OK";
      if (!caption.empty()) report += " (CAPTION=" + caption + ")";
      report += "\n";
    } else {
      report += "Gameexe: Gameexe.ini not found (even after case correction)\n";
    }

    // --- SEEN.TXT ---
    const fs::path seen_path = CorrectPathCase(root / "Seen.txt");
    if (!seen_path.empty()) {
      libreallive::Archive archive(seen_path.string());
      // ReadTOC() 把 (offset, length) 表映射成场景索引；offset 为 0 的条目不存在，
      // 所以索引不一定从 0 开始。这里直接遍历 TOC 拿到真实索引。
      int scenario_count = 0;
      std::string indices;
      for (libreallive::Archive::const_iterator it = archive.begin();
           it != archive.end(); ++it) {
        ++scenario_count;
        if (scenario_count <= 8) {
          if (!indices.empty()) indices += ",";
          indices += std::to_string(it->first);
        }
      }
      report += "Seen: TOC entries=" + std::to_string(scenario_count) +
                "; indices=[" + indices + "]";

      // 构造第一个场景会真正走通解压路径（compression.cc）。
      libreallive::Scenario* scenario =
          (archive.begin() != archive.end())
              ? archive.GetScenario(archive.begin()->first)
              : nullptr;
      report += std::string("; firstScenario=") +
                (scenario != nullptr ? "constructed" : "null");
      report += "; probableEncoding=" +
                std::to_string(archive.GetProbableEncodingType());
      report += "\n";
    } else {
      report += "Seen: Seen.txt not found (even after case correction)\n";
    }

    report += "PROBE OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "probe report", report);
  return NewSafeJavaString(env, report);
}

/**
 * 在真机上真正跑起引擎：装配 AndroidSystem + RLMachine + 全部指令模块，
 * 然后执行字节码，直到停机、进入长操作或耗尽指令预算。
 *
 * 这是 T2.4（引擎生命周期）的第一步——只跑不控。启动/暂停/恢复/退出
 * 的完整生命周期要等 AndroidSystem 接入渲染与事件源之后再补。
 */
/**
 * 装配 AndroidSystem + RLMachine 并执行字节码。
 * 「普通路径」与「SAF」两个入口共用这段逻辑，差异只在 Archive 怎么打开。
 */
/**
 * 汉化支持（见 docs/LOCALIZATION.md）：把每个场景的文本串按顺序导出成 TSV。
 *
 * 导出顺序 = 「场景号升序、场景内出现顺序」。这个顺序必须可靠，因为它是后续
 * 与中文译文对齐的唯一依据；序章「心臓がドキドキ…」应能在结果里找到。
 */
void ExportSceneText(libreallive::Archive& archive,
                     const std::string& out_dir,
                     std::string& report) {
  if (out_dir.empty()) {
    report += "export_jp_text: no diagnostics dir; skipped\n";
    return;
  }

  // 诊断：常驻内存探针 + 自身上限。真机上出现过「解析到一半内存暴涨、被系统
  // lowmemorykiller SIGKILL，连带把别的应用一起杀」的情况——这里把内存曲线和
  // 每个场景的元素数打进日志，并在超过上限时主动停手，避免再把整机拖下水。
  LogMemory("export start");
  std::ostringstream out;
  int scenes = 0;
  int strings = 0;
  std::vector<int> failed;
  for (auto it = archive.begin(); it != archive.end(); ++it) {
    const int index = it->first;
    const long rss_before = CurrentRssKb();
    if (rss_before > kMemoryGuardKb) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "export_jp_text: aborted before scene %d, rss=%ld kB > %ld kB",
                          index, rss_before, kMemoryGuardKb);
      report += "export_jp_text: aborted at scene " + std::to_string(index) +
                " (rss guard)\n";
      break;
    }
    // 逐场景容错：解析失败（例如汉化补丁改写过的菜单场景，select 结构不是
    // RLVM 期望的布局）只跳过这一个场景并记账，否则一个坏场景会让整份导出
    // ——以及"到底哪些场景坏"这个诊断结论——全都拿不到。
    libreallive::Scenario* scenario = nullptr;
    try {
      scenario = archive.GetScenario(index);
    } catch (const std::exception& e) {
      failed.push_back(index);
      report += "export_jp_text: scene " + std::to_string(index) +
                " parse failed: " + e.what() + "\n";
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "export_jp_text: scene %d parse failed: %s", index,
                          e.what());
      continue;
    }
    if (scenario == nullptr) continue;
    ++scenes;

    // 该场景自己声明的文本编码：0 = CP932（默认），1 = CP936/GBK。
    // 语义与渲染路径完全一致（RLMachine::GetTextEncoding() 就是
    // call_stack_.back().scenario->encoding()），**不能**在 0 时回退到
    // Archive::GetProbableEncodingType()——那是整库默认值，用在逐场景上会把
    // 未覆盖的日文场景（声明 0）也按 GBK 解成乱码。
    const int encoding = scenario->encoding();

    // 元素数：正常场景是「几千」量级；被改坏的数据会让扫描器一路错位，把整段
    // 数据切成十万级甚至百万级元素——这就是内存暴涨的直接来源，先量出来。
    int elements = 0;
    for (auto element = scenario->begin(); element != scenario->end(); ++element) {
      ++elements;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "export scene %d: dlen=%zu elements=%d enc=%d rss=%ld kB",
                        index, it->second.length, elements, encoding, rss_before);

    int ordinal = 0;
    for (auto element = scenario->begin(); element != scenario->end(); ++element) {
      // BytecodeList 是 forward_list<unique_ptr<BytecodeElement>>，取 get() 判型。
      const libreallive::TextoutElement* text =
          dynamic_cast<const libreallive::TextoutElement*>((*element).get());
      if (text == nullptr) continue;
      // 按场景声明的编码转成 UTF-8，便于比对与显示。
      out << index << '\t' << ordinal++ << '\t'
          << cp932toUTF8(text->GetText(), encoding)
          << '\n';
      ++strings;
    }
  }

  const std::string path = out_dir + "/jp-text.tsv";
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    report += "export_jp_text: cannot write " + path + "\n";
    return;
  }
  const std::string payload = out.str();
  file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  file.close();
  report += "export_jp_text: scenes=" + std::to_string(scenes) +
            " strings=" + std::to_string(strings) +
            " failed=" + std::to_string(failed.size()) + " -> " + path + "\n";
}

namespace {

// RealLive 的 Refresh 模块（module 1:31）有两个重载：
//   0 = refresh —— 强制出一帧（上游 RLVM 已实现）
//   1 =         —— 等到下一帧（**上游没实现**）
// LB / LBEX 的小游戏主循环 SEEN7100 第 33/37 行两个都在用；缺了重载 1，脚本会
// 在那条指令上死循环重试（logcat 里刷 `Undefined: opcode<1:31:1, 0>`），
// 表现就是画面停住、只剩 TIME 之类的静态元素。
//
// 上游 `modules/` 目录受保护，而且 RLMachine::modules_ 是 private、没有访问器，
// 所以这里在**平台层**补：继承 RLMachine 并重写 virtual 的 AttachModule，
// 在 Refresh 模块被挂上去之前给它加一条指令。
//
// 「等一帧」用 LongOperation 实现：返回 false 时机器停在这条指令上，
// 而平台外层循环每轮都会合帧再回来，所以等价于「等到至少 kRefreshWaitFrameMs 过去」。
// 注意别取 16：演示段（SEEN7100）的 ex-frames 计数器步长是
// `milliseconds / |frame_max - frame_min|` = 1000/60 = 16.67ms，脚本要跑满 60 帧
// （1000ms）才判定「演示结束」；用 16ms 的话 60 帧只用 960ms，第 60 次检查时
// 计数器还活着 → 循环不退出、外层重新 InitExFrames → 无限循环（真机实测）。
// 取 17ms（60 帧 = 1020ms）留出余量。
const unsigned int kRefreshWaitFrameMs = 17;  // ~59fps，刻意略慢于计数器步长

class RefreshWaitLongOp : public LongOperation {
 public:
  explicit RefreshWaitLongOp(unsigned int until) : until_(until) {}

  bool operator()(RLMachine& machine) override {
    const unsigned int now = machine.system().event().GetTicks();
    if (now < until_) return false;
    machine.system().graphics().ForceRefresh();
    return true;
  }

 private:
  unsigned int until_;
};

struct RefreshWaitOp : public RLOp_Void_Void {
  void operator()(RLMachine& machine) override {
    const unsigned int now = machine.system().event().GetTicks();
    machine.PushLongOperation(new RefreshWaitLongOp(now + kRefreshWaitFrameMs));
    machine.AdvanceInstructionPointer();
  }
};

// ---------------------------------------------------------------------------
// LB 小游戏需要、而上游 RLVM 从未实现的几条脚本指令（见 dev-log/OPEN-ISSUES.jsonl）。
// 每帧循环会撞它们，撞一次就跳一次、循环推进不下去，表现出来就是画面停住。
// ---------------------------------------------------------------------------

// 参数元数不固定的指令（Sys 151 有 9 个参数、Sys 152 有 11 个）：走上游给
// 「特殊情形」准备的逃生口，**不解析参数**直接吞掉，避免把后续字节码喂错。
// 语义待反推；先让它别卡，并把实参原样打进日志供分析。
// Sys 151 / 152：LBEX 小游戏的**输入轮询**（把「当前按下的鼠标键 / 键位」写进脚本
// 给的引用里）。脚本原文：
//     SEEN7310: op<1:4:151, 0>(intD[101], 5, 4, 49, 0, 1, 2, 3, 100)
//     SEEN7310: op<1:4:152, 0>(intD[109], 81..89, 80)
//     SEEN7420: if (intD[101] == 1) { intL[0] = objbtn_select(); if (intL[0] < 0) 挥棒 }
// 也就是说小游戏的「挥棒」完全依赖 intD[101] 被每帧刷新。原来我们把它当「忽略」
// 丢掉了 → intD[101] 恒为 0 → 挥棒分支永不执行，而且帧循环也走不下去。
// 参数元数不定，所以和 LbIgnoreRawArgs 一样走 SpecialCase：自己解析第一个参数（引用）。
class LbInputPollOp : public RLOp_SpecialCase {
 public:
  explicit LbInputPollOp(bool mouse) : mouse_(mouse) {}

  // 必须真的把第一个参数解析成**引用**（脚本写的是 intD[101]）。
  // 空实现会让 params 为空 —— 执行期的 IntReference_T::getData 拿不到东西，
  // `*ref = v` 就写到别处。真机表现正是「op 每帧都在跑、intD[101] 却恒为 0」。
  void ParseParameters(const std::vector<std::string>& input,
                       libreallive::ExpressionPiecesVector& output) override {
    unsigned int pos = 0;
    IntReference_T::ParseParameters(pos, input, output);
  }

  void operator()(RLMachine& machine,
                  const libreallive::CommandElement& f) override {
    const libreallive::ExpressionPiecesVector& params = f.GetParsedParameters();
    if (!params.empty()) {
      unsigned int pos = 0;
      IntReferenceIterator ref = IntReference_T::getData(machine, params, pos);
      int value = 0;
      if (mouse_) {
        // 脚本判挥棒用的是 `intD[101] == 1`（见 SEEN7420），所以这里必须报「按下」：
        //   * 当前还按着 -> 1；
        //   * 本帧刚按下过（哪怕已经抬起）-> 也要报 1（快速点击的按下状态只存在几毫秒，
        //     按「当前电平」轮询会整帧错过；真机就是这样一直卡在 `== 1` 不成立）。
        // 其余情况写 0（不是 2：2 是「已松开」，脚本那边不是这个语义）。
        Point cursor;
        int b1 = 0, b2 = 0;
        machine.system().event().GetCursorPos(cursor, b1, b2);
        const AndroidEventSystem& es =
            static_cast<const AndroidEventSystem&>(machine.system().event());
        value = (b1 == 1 || es.ClickSeenThisFrame()) ? 1 : 0;
      }
      // Sys 152（键盘）在全脚本里只写不读（intD[109] 无任何使用点），故先恒 0。
      *ref = value;
      static int logged = 0;
      if (logged < 8) {
        ++logged;
        std::cout << "[lb-ext] input poll " << (mouse_ ? "mouse" : "key")
                  << " -> " << value << " (ref " << ref.type() << ":"
                  << ref.location() << ")" << std::endl;
      }
    }
    machine.AdvanceInstructionPointer();
  }

 private:
  bool mouse_;
};

class LbIgnoreRawArgs : public RLOp_SpecialCase {
 public:
  LbIgnoreRawArgs(int module_number, int opcode, const char* label)
      : module_number_(module_number), opcode_(opcode), label_(label) {}

  void ParseParameters(const std::vector<std::string>&,
                       libreallive::ExpressionPiecesVector&) override {}

  // **必须覆写 `Dispatch`**：有些 op（如 ChildObjFg 的 `2:81:1058`）不走
  // `RLOp_SpecialCase::DispatchFunction`，而走 `module_obj.cc` 的
  // `handler->Dispatch(machine, ...)`；基类 `Dispatch` 直接
  // `throw Exception("Tried to call empty RLOp_SpecialCase::Dispatch().")`
  // —— 真机日志 `(SEEN7340)(Line 110)[lb_child_obj_1058]: Tried to call empty …`
  // 就是它：每帧抛异常 + 每帧一行日志，而这个 op 的效果从未落地。
  // 这里做成真正的静默 no-op（与"忽略"同义，但不再每帧报错）。
  void Dispatch(RLMachine& machine,
                const libreallive::ExpressionPiecesVector&) override {
    static std::set<int> logged;
    const int key = module_number_ * 100000 + opcode_;
    if (logged.insert(key).second) {
      std::cout << "[lb-ext] " << label_ << " (module 1:" << module_number_
                << " op " << opcode_ << ") ignored (via Dispatch)" << std::endl;
    }
    machine.AdvanceInstructionPointer();
  }
  void operator()(RLMachine& machine,
                  const libreallive::CommandElement& f) override {
    static std::set<int> logged;
    const int key = module_number_ * 100000 + opcode_;
    if (logged.insert(key).second) {
      std::cout << "[lb-ext] " << label_ << " (module 1:" << module_number_
                << " op " << opcode_
                << ") not implemented yet; args ignored" << std::endl;
    }
    machine.AdvanceInstructionPointer();
  }

 private:
  int module_number_;
  int opcode_;
  const char* label_;
};

// 3 个 int 参数的占位实现（2:87:1002 的形状是「父对象, 子序号, 值」）。
// ---------------------------------------------------------------------------
// Pcm(1:21) 1000：**按名字把音效预载进槽位**
//
// LBEX 的小游戏一进场景就调 36 次 `op<1:21:1000>(槽位, "PT_xxx")`，名字是
// `WAV/` 下的 Ogg 音效（PT_BOUND01=弹地、PT_BAT01=球棒、PT_HIT00=击中、
// PT_CUTIN00=切入…；槽位 1..36，缺 10/26）。RLVM 的 `wavPlay` 只接受**文件名**
// （播放时按需加载），所以这里不必真的预载，只要把映射记下来：既消掉
// 「Undefined opcode」噪声，也为「万一以后按槽位播放」留一张表。
std::map<int, std::string>& LbWavSlotTable() {
  static std::map<int, std::string> table;
  return table;
}

struct LbWavLoadByName : public RLOp_Void_2<IntConstant_T, StrConstant_T> {
  void operator()(RLMachine& machine, int slot, std::string name) {
    LbWavSlotTable()[slot] = name;
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::cout << "[lb-ext] Pcm(1:21):1000 按名预载音效，槽位表已建立（示例 "
                << slot << " = " << name << "）" << std::endl;
    }
    machine.AdvanceInstructionPointer();
  }
};

// ChildObjFg(2:81) 1058：LBEX 小游戏给**子对象**设属性（参数里带实体记录字段 +25）。
// RLVM 的对象函数表在这一段是断的（1038 之后直接跳到 1064），映射表里没有 1058。
// 这里先「记录参数 + 忽略」：RLVM 对未知 opcode 本来就是跳过，所以推进不受影响，
// 但把取值打出来，供以后按真实语义实现。
struct LbChildObj1058
    : public RLOp_Void_3<IntConstant_T, IntConstant_T, IntConstant_T> {
  void operator()(RLMachine& machine, int a, int b, int c) {
    static std::set<std::string> logged;
    std::ostringstream os;
    os << a << "," << b << "," << c;
    if (logged.size() < 12 && logged.insert(os.str()).second) {
      std::cout << "[lb-ext] ChildObjFg(2:81):1058(" << a << ", " << b << ", "
                << c << ") 暂按忽略处理（记录备查）" << std::endl;
    }
    machine.AdvanceInstructionPointer();
  }
};

struct LbStub3 : public RLOp_Void_3<IntConstant_T, IntConstant_T, IntConstant_T> {
  void operator()(RLMachine& machine, int a, int b, int c) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::cout << "[lb-ext] obj(2:87):1002(父,子,值) not implemented yet"
                << std::endl;
    }
  }
};

// Sel 模块（0:2）的 objbtn 组：RLVM 只实现了 objbtn_init(20) 与
// select_objbtn(4)/select_objbtn_cancel(14)，而 LB / LBEX 的小游戏用的是
// 21 / 22 / 23 / 30 / 32 —— 缺了它们，脚本里「等玩家点按钮再推进相位」那一步
// 永远拿不到结果（`<store>` 恒为 0），于是主循环空转、表现就是 TIME 一直重来。
//
// 形状对照（全库取证）：30/32 与已经实现的 14 完全同形 —— 无参数、紧跟
// `<store>` 取回一个按钮下标、随后判断 `< 0` 或 `>= -1`。所以它们是同类
// 「等对象按钮点击并返回下标」的阻塞式选择。组号从 `00022, 1(group, value)`
// （脚本里只出现 group=2）里取；21/23 是配置类，先按空实现接上。
int g_objbtn_group = 2;

// 小游戏每帧诊断日志的开关（默认关）。
// 下面 objbtn 那几行是**每帧同步写 stdout → logcat**；实测这类逐帧日志会把小游戏
// 越跑越慢（同 pt00 的逐调用日志，见 dev-log/PT00-EMU.jsonl 的 perf 记录）。
// 需要时用 diag 的 pt00_verbose=1 打开。
bool g_lb_verbose = false;
void SetLbVerbose(bool on) { g_lb_verbose = on; }

namespace {
class LbNullBuf : public std::streambuf {
 protected:
  int overflow(int c) override { return c; }
};
}  // namespace

/** 默认丢弃的小游戏诊断流；g_lb_verbose 时才真的往 stdout 写。 */
std::ostream& LbLog() {
  static LbNullBuf buf;
  static std::ostream null_stream(&buf);
  return g_lb_verbose ? std::cout : null_stream;
}

struct ObjBtnConfig2 : public RLOp_Void_2<IntConstant_T, IntConstant_T> {
  void operator()(RLMachine& machine, int group, int value) {
    g_objbtn_group = group;
  }
};

struct ObjBtnNoop : public RLOp_Void_Void {
  void operator()(RLMachine& machine) {}
};

// 30 / 32：等对象按钮点击，把被点按钮的下标写进 store 寄存器
// （cancelable 的那条对应上游的 select_objbtn_cancel）。
struct ObjBtnSelect : public RLOp_Store_Void {
  explicit ObjBtnSelect(bool cancelable) : cancelable_(cancelable) {}
  int operator()(RLMachine& machine) override {
    if (machine.ShouldSetSelcomSavepoint()) machine.MarkSavepoint();
    // 诊断：把「这一组里到底有几个按钮、各自在哪」打出来 —— 小游戏卡住时
    // 最常见的原因就是按钮组是空的（对象没标成按钮 / 组号不对），或者点位不对。
    {
      int fg = 0, btns = 0, idx = -1;
      int hit = -1;  // 本帧鼠标点中的组内按钮编号（-1 = 点在非按钮处）
      GraphicsSystem& g = machine.system().graphics();
      for (GraphicsObject& o : g.GetForegroundObjects()) {
        ++fg; ++idx;
        if (o.IsButton() && o.GetButtonGroup() == g_objbtn_group) {
          ++btns;
          Rect r = o.has_object_data()
                       ? o.GetObjectData().DstRect(o, nullptr)
                       : Rect();
          LbLog() << "[lb-ext] objbtn hit#" << btns << " obj=" << idx
                    << " group=" << o.GetButtonGroup() << " rect=(" << r.x()
                    << "," << r.y() << " " << r.width() << "x" << r.height()
                    << ") show=" << o.visible() << std::endl;
        } else if (o.has_object_data()) {
          ParentGraphicsObjectData* parent =
              dynamic_cast<ParentGraphicsObjectData*>(&o.GetObjectData());
          if (parent) {
            for (GraphicsObject& c : parent->objects()) {
              if (c.IsButton() && c.GetButtonGroup() == g_objbtn_group) {
                ++btns;
                Rect r = c.GetObjectData().DstRect(c, &o);
                LbLog() << "[lb-ext] objbtn hit#" << btns << " child of obj="
                          << idx << " group=" << c.GetButtonGroup()
                          << " rect=(" << r.x() << "," << r.y() << " "
                          << r.width() << "x" << r.height() << ")"
                          << std::endl;
              }
            }
          }
        }
      }
      // 命中判定：鼠标当前按着时，落在哪个组内按钮的屏幕矩形里。
      Point pos;
      int b1 = 0, b2 = 0;
      machine.system().event().GetCursorPos(pos, b1, b2);
      AndroidEventSystem& es =
          static_cast<AndroidEventSystem&>(machine.system().event());
      const bool pressed = (b1 == 1) || es.ClickSeenThisFrame();
      if (pressed) {
        if (es.ClickSeenThisFrame()) pos = es.ClickPosition();
        for (GraphicsObject& o : g.GetForegroundObjects()) {
          auto consider = [&](GraphicsObject& b, GraphicsObject* parent) {
            if (!b.IsButton() || b.GetButtonGroup() != g_objbtn_group) return;
            if (!b.has_object_data()) return;
            Rect r = b.GetObjectData().DstRect(b, parent);
            if (r.Contains(pos)) hit = b.GetButtonNumber();
          };
          consider(o, nullptr);
          if (o.has_object_data()) {
            ParentGraphicsObjectData* parent =
                dynamic_cast<ParentGraphicsObjectData*>(&o.GetObjectData());
            if (parent) {
              for (GraphicsObject& c : parent->objects()) consider(c, &o);
            }
          }
        }
      }
      LbLog() << "[lb-ext] objbtn poll group=" << g_objbtn_group
                << " at=" << pos.x() << "," << pos.y() << " b1=" << b1
                << " b2=" << b2 << " fg_objects=" << fg
                << " buttons_in_group=" << btns << " -> " << hit << std::endl;
      // 就在「要等玩家点选」这一刻把图形栈转储出来：小游戏卡住时画面到底有什么、
      // 每个对象的源/目标矩形是不是 0×0，一望便知（引擎卡着不会走到退出路径的 dump）。
      static int tree_dumps = 0;
      if (tree_dumps < 2) {
        ++tree_dumps;
        std::ostringstream tree;
        g.Refresh(&tree);
        std::cout << "[lb-ext] graphics tree:" << std::endl << tree.str() << std::endl;
        // 同一刻把脚本侧 intD 整片打出来 —— 给「PC pt00_probe --full」逐项对照用
        // （小游戏的相机/相位差异都在这块数组里）。
        pt00emu::DumpIntD(machine);
      }
      return hit;  // 非阻塞：本帧直接给出结果（-1 = 点在非按钮处 = 脚本判「挥棒」）
    }
  }

 private:
  bool cancelable_;
};

// 上游没有的模块族 2:87（脚本用它设置「父对象的某个子对象」的状态）。
class LbObjExtModule : public RLModule {
 public:
  LbObjExtModule() : RLModule("LbObjExt", 2, 87) {
    AddOpcode(1002, 0, "childSetState", new LbStub3());
  }
};

// 平台层扩展：给 (1,31) 补上 refresh 的重载 1、(1,11) 补上 Mem 201、
// (1,4) 补上 Sys 151/152）；其余模块原样交给上游。
class AndroidRLMachine : public RLMachine {
 public:
  using RLMachine::RLMachine;

  void AttachModule(RLModule* module) override {
    if (module != nullptr && module->module_type() == 1 &&
        module->module_number() == 31) {
      module->AddOpcode(1, 0, "refresh_wait_frame", new RefreshWaitOp());
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 11) {
      // Mem 201：`Mem201(个数, 键数组, 下标数组)`。全库仅一处调用，从消费端
      // （`farcall(7500, 2, {intA[i]})` 把值当**实体下标**用）反推，它应该是
      // 「按键数组给下标数组排序」的 argsort。真实现要用上游的
      // IntReference_T/IntReferenceIterator（还没接对头），这里先吞掉：
      // 脚本调用前已把 intA[0..21] 填成 0..21，忽略它 = 保持下标顺序，
      // z 序可能不对但循环不再卡死。
      module->AddOpcode(201, 0, "lb_ignore_mem201",
                        new LbIgnoreRawArgs(11, 201, "Mem201"));
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 4) {
      // 151/152 = LBEX 的输入轮询（见 LbInputPollOp 的注释）：必须真的写回引用，
      // 否则小游戏的「挥棒」判定（intD[101] == 1）永远不成立。
      module->AddOpcode(151, 0, "lb_input_mouse", new LbInputPollOp(true));
      module->AddOpcode(152, 0, "lb_input_key", new LbInputPollOp(false));
      // LBEX 小游戏/演出还用了一批 RLVM 没登记的 Sys 号（真机日志实测）：
      // 150/210/211/215/216 出现在 SEEN7110/515，436/441/446/451/456 出现在 SEEN7010。
      // 一律容错占位（不解析参数 + 自己推进 IP），先让脚本不被它们挡住；
      // 其中 456 在脚本里是 overload 1（其余是 0）。
      module->AddOpcode(150, 0, "lb_ignore_150",
                        new LbIgnoreRawArgs(4, 150, "Sys150"));
      module->AddOpcode(210, 0, "lb_ignore_210",
                        new LbIgnoreRawArgs(4, 210, "Sys210"));
      module->AddOpcode(211, 0, "lb_ignore_211",
                        new LbIgnoreRawArgs(4, 211, "Sys211"));
      module->AddOpcode(215, 0, "lb_ignore_215",
                        new LbIgnoreRawArgs(4, 215, "Sys215"));
      module->AddOpcode(216, 0, "lb_ignore_216",
                        new LbIgnoreRawArgs(4, 216, "Sys216"));
      module->AddOpcode(436, 0, "lb_ignore_436",
                        new LbIgnoreRawArgs(4, 436, "Sys436"));
      module->AddOpcode(441, 0, "lb_ignore_441",
                        new LbIgnoreRawArgs(4, 441, "Sys441"));
      module->AddOpcode(446, 0, "lb_ignore_446",
                        new LbIgnoreRawArgs(4, 446, "Sys446"));
      module->AddOpcode(451, 0, "lb_ignore_451",
                        new LbIgnoreRawArgs(4, 451, "Sys451"));
      module->AddOpcode(456, 1, "lb_ignore_456",
                        new LbIgnoreRawArgs(4, 456, "Sys456"));
      // 真机日志里实际撞到的 Undefined（引擎自己打的 `Undefined: opcode<1:4:NNN, K>`
      // 就是"确不存在"的证明）+ tools/sys_opcode_coverage.py 的审计结果。
      // 一律按同一做法挂容错桩：效果与"引擎跳过"相同，但不再刷日志、也不再走异常路径。
      // 其中 **460 / 461（最频繁）与 2056 / 3503 / 300 系列语义未知**，
      // 需要按 D-040 用真值标定后再实现（已记进 docs/MINIGAME-STATIC-RECON.md）。
      static const struct { int op, ov; } kSysIgnore[] = {
          {106, 0},  {200, 0},  {201, 0},  {300, 0},  {301, 0}, {302, 0},
          {366, 0},  {432, 0},  {437, 0},  {442, 0},  {447, 0}, {452, 0},
          {456, 0},  // 原来只注册了 overload 1 —— 这是漏项
          {457, 0},  {457, 1},  {460, 1},  {461, 1},  {711, 0}, {713, 0},
          {801, 0},  {1231, 0}, {2056, 0}, {3503, 0},
      };
      for (const auto& e : kSysIgnore) {
        module->AddOpcode(e.op, e.ov, "lb_ignore_sys_ext",
                          new LbIgnoreRawArgs(4, e.op, "SysExt"));
      }
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 255) {
      // Debug 模块：LBEX 脚本用 12/14/15，RLVM 的 DebugModule 没实现 → 真机日志里是
      // Undefined。按同一做法挂容错桩（本来就是调试用，无语义副作用）。
      for (const int op : {12, 14, 15}) {
        module->AddOpcode(op, 0, "lb_ignore_debug",
                          new LbIgnoreRawArgs(255, op, "Debug"));
      }
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 73) {
      // GanFg（立绘动画）：RLVM 只有 3/4/104/1000..1007，而 LBEX 脚本用 101/102。
      // ⚠️ 这两条**很可能是"实体不渲染"的源头之一**（动画/显示控制）—— 这里只做到
      // "不再刷 Undefined"的容错；真语义要按 D-040 用真值标定后实现。
      for (const int op : {101, 102}) {
        module->AddOpcode(op, 0, "lb_ignore_ganfg",
                          new LbIgnoreRawArgs(73, op, "GanFg"));
      }
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 12) {
      // Syscom：RLVM 有 1210..1216，LBEX 还用 1103(overload 1)。
      module->AddOpcode(1103, 1, "lb_ignore_syscom_1103",
                        new LbIgnoreRawArgs(12, 1103, "Syscom1103"));
    } else if (module != nullptr && module->module_type() == 1 &&
               module->module_number() == 21) {
      // Pcm：小游戏按名预载音效（WAV/PT_*.ogg），见 LbWavLoadByName 注释。
      // 用容错占位：LBEX 里同号 opcode 的参数形状不唯一（吃过一次崩），
      // 一律「不解析参数 + 自己推进 IP」，和 Mem201/Sys151/152 同一条路。
      module->AddOpcode(1000, 0, "lb_wav_load_by_name",
                        new LbIgnoreRawArgs(21, 1000, "Pcm按名预载音效"));
    } else if (module != nullptr && module->module_type() == 2 &&
               module->module_number() == 81) {
      // ChildObjFg：LBEX 小游戏用的子对象属性（RLVM 表里缺 1058）。
      module->AddOpcode(1058, 1, "lb_child_obj_1058",
                        new LbIgnoreRawArgs(81, 1058, "ChildObjFg属性"));
    } else if (module != nullptr && module->module_type() == 0 &&
               module->module_number() == 2) {
      // Sel 模块缺的 objbtn 组（见上面的长注释）。
      module->AddOpcode(21, 0, "objbtn_21", new ObjBtnNoop());
      module->AddOpcode(22, 1, "objbtn_cfg", new ObjBtnConfig2());
      module->AddOpcode(23, 0, "objbtn_23", new ObjBtnNoop());
      module->AddOpcode(30, 0, "objbtn_select", new ObjBtnSelect(false));
      module->AddOpcode(32, 0, "objbtn_select_cancel", new ObjBtnSelect(true));
    }
    RLMachine::AttachModule(module);
  }
};

}  // namespace

void RunEngineOn(System& system,
                 Gameexe& gameexe,
                 libreallive::Archive& archive,
                 int max_instructions,
                 std::string& report) {
  // 分步日志：真机上「跑很久却没有任何输出」时，用它定位卡在哪一步。
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: constructing RLMachine");
  LogMemory("engine start");

  // 设备侧诊断参数（文件不存在时全部取缺省值，行为与之前一致）。
  // 以前这行在 AddGameHacks 之后，但小游戏开关要在注册游戏 hack 之前就知道。
  const DiagOptions diag = LoadDiagOptions();

  // 诊断：dump_scenes=7300,7450 → 把这几幕的脚本反汇编打到日志（引擎启动就做，
  // 不需要真的走到那一幕）。用来读「小游戏那几段循环到底在等什么」。
  // 取证开关：让执行器记录对 ctx 块 / 低地址的读写（看 DLL 在找哪个引擎数组）
  pt00emu::SetTraceCtx(diag.pt00_trace_ctx);
  pt00emu::SetTick31(diag.pt00_tick31);
  pt00emu::SetVerbose(diag.pt00_verbose);
  SetLbVerbose(diag.pt00_verbose);
  /* max_lines = -1：只进环形缓冲，步数上限时才整环转储（避免每帧几十条刷屏）。 */
  if (diag.pt00_watch != 0)
    pt00emu::SetWatch(diag.pt00_watch, -1);
  if (!diag.dump_scenes.empty()) {
    // `dump_scenes=all` → 把 **全部** 场景反汇编写文件（351 幕约 35MB，走 logcat 必爆缓冲）。
    // 其余写法是逗号分隔的场景号，同样写文件（但只有列出的那几幕）。
    const bool dump_all = (diag.dump_scenes == "all");
    std::ofstream scene_file;
    std::streambuf* old_cout_buf = nullptr;
    if (dump_all) {
      const char* home = getenv("HOME");
      const std::string dir =
          home ? std::string(home) : std::string("/sdcard/Android/data/org.rlvm.android/files");
      const std::string path = dir + "/rlvm-scenes.txt";
      scene_file.open(path.c_str(), std::ios::out | std::ios::trunc);
      if (scene_file) {
        old_cout_buf = std::cout.rdbuf(scene_file.rdbuf());
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "dump_scenes=all -> %s",
                            path.c_str());
        std::cerr << "[scenes] 全幕反汇编 -> " << path << std::endl;
      } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "dump_scenes=all: 打不开输出文件，退回逐幕打印");
      }
    }
    std::vector<int> ids;
    if (dump_all) {
      for (int i = 0; i < 10000; ++i) ids.push_back(i);
    } else {
      std::istringstream scs(diag.dump_scenes);
      std::string tok;
      while (std::getline(scs, tok, ',')) ids.push_back(std::atoi(tok.c_str()));
    }
    for (int id : ids) {
      std::cout << "===== SEEN" << id << " =====" << std::endl;
      std::cout << std::flush;
      try {
        libreallive::Scenario* scene = archive.GetScenario(id);
        if (scene) {
          DumpScenario(scene);
        } else {
          if (!dump_all) {
            // 单幕模式才打这句；全幕模式里空条目是常态，不刷屏。
            std::cout << "(GetScenario(" << id << ") 返回空：TOC 里没有该条目，或构造失败)"
                      << std::endl;
          }
        }
        std::cout << std::flush;
      } catch (const std::exception& e) {
        std::cout << "(取/转储 SEEN" << id << " 时抛 std 异常: " << e.what() << ")"
                  << std::endl;
      } catch (...) {
        // 关键：DumpScenario 在个别幕上会抛非 std 异常，之前它会把整个引擎带走、
        // 连已缓冲的输出都丢掉（这份诊断因此骗过我一次）。这里兜住并继续下一幕。
        std::cout << "(取/转储 SEEN" << id << " 时抛未知异常，跳过这一幕)"
                  << std::endl;
      }
      if (!dump_all) {
        std::cout << "===== SEEN" << id << " 结束 =====" << std::endl;
      }
    }
    if (old_cout_buf) {
      std::cout.rdbuf(old_cout_buf);
      std::cerr << "[scenes] 全幕反汇编完成" << std::endl;
    }
  }

  // 触摸输入（T2.3）：把当前系统暴露给 UI 线程，离开 RunEngineOn 时自动断开。
  CurrentSystemGuard current_system(dynamic_cast<AndroidSystem*>(&system));

  // 平台层扩展过的机器：它会补上 Refresh 模块缺的重载 1（见上面的 AndroidRLMachine）。
  AndroidRLMachine machine(system, archive);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddAllModules");
  AddAllModules(machine);
  // 上游没有的模块族 2:87（LB 小游戏的「设子对象状态」），由平台层补上。
  machine.AttachModule(new LbObjExtModule());
  // 试验：小游戏相位标志。脚本 `SEEN7110:0087/0154` 读 `intG[1900]/[1901]` 决定
  // 要不要进入「可操作阶段」，而全库没人写它们（整段 trace 零次写入、DLL 也只碰
  // ctx+0x14）——推断这是**原引擎在小游戏周围做的胶水**，RLVM 的 LB_SkipBaseball
  // hack 把它整段绕过了。脚本在第 87 行就检查，所以必须**引擎启动时**置位。
  if (diag.lb_minigame) {
    machine.SetIntValue(
        libreallive::IntMemRef(libreallive::INTG_LOCATION, 1900), 1);
    machine.SetIntValue(
        libreallive::IntMemRef(libreallive::INTG_LOCATION, 1901), 1);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "lb_minigame: intG[1900]/[1901] 试验置 1（原引擎小游戏胶水）");
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: AddGameHacks");
  // lb_minigame=1 且本作是 LB/LBEX 时，不注册那个「直接 ReturnFromFarcall
  // 跳过棒球小游戏」的 line action（上游 game_hacks.cc:65），让小游戏真跑。
  LittleBustersPT00DLL::SetCallLogging(diag.lb_minigame);
  bool skip_game_hacks = false;
  if (diag.lb_minigame) {
    const std::string diskmark = gameexe("DISKMARK").ToString("");
    skip_game_hacks = (diskmark == "LB.ENV" || diskmark == "LB_EX.ENV");
    if (skip_game_hacks) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "lb_minigame=1: baseball skip hack disabled (%s)",
                          diskmark.c_str());
      report += "lb_minigame=1：已关闭 LB 棒球跳过 hack（diskmark=\"" +
                diskmark + "\"），脚本会进入 SEEN7030\n";
    } else {
      report += "lb_minigame=1：diskmark=\"" + diskmark +
                "\" 不是 LB/LBEX，游戏 hack 照常注册\n";
    }
  }
  if (!skip_game_hacks) AddGameHacks(machine);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "step: machine ready");
  LogMemory("machine ready");
  // 与上游 RLVMInstance 一致：遇到指令异常时跳过该指令继续执行，
  // 而不是把整台机器标记为 halted。
  machine.SetHaltOnException(false);
  // 打印未实现的操作码与指令异常：上游默认把它们吞掉，
  // 在 Android 上 std::cerr 又看不到，于是表现为「跑了很多指令却什么都没发生」。
  machine.SetPrintUndefinedOpcodes(true);

  // 与上游 RLVMInstance 一致：启动时载入"全局数据"（Config：音量、文字速度、
  // 画面设置等都在里面）。之前我们绕过了 RLVMInstance，这两步就漏掉了——
  // 结果是配置写得下去却永远读不回来，表现为"config 不保留"。
  try {
    Serialization::loadGlobalMemory(machine);
    report += "global memory loaded\n";
  } catch (const std::exception& e) {
    // 首次运行没有 global.sav.gz，属于正常情况。
    report += std::string("global memory not loaded (first run?): ") + e.what() + "\n";
  }

  g_frame_log_every = diag.frame_log_every;
  g_blit_cost_log = diag.blit_cost;
  rlvm_android::SetLbWipeLog(diag.wipe_log);
  rlvm_android::SetLbOpTraceFilter(diag.op_trace);
  rlvm_android::SetLbCaseTrace(diag.case_trace);
  rlvm_android::SetLbPatNoTrace(diag.patno_trace);
  if (!diag.op_trace.empty()) machine.set_tracing_on();
  g_touch_buttons.store(diag.touch_button);
  SetBlitStatsEnabled(diag.blit_stats);
  SetBlitFastEnabled(diag.blit_fast);
  SetDirtyGateEnabled(diag.dirty_gate);

  // v0.2.3 影片探针（M2）：只跑一次，把「PS 解复用 + AMediaCodec」的结果写进报告，
  // 不影响引擎本身的运行。见 android/mov_probe.h。
  if (!diag.mov_probe.empty()) {
    std::string mov_out_dir;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      mov_out_dir = g_diag_dir;
    }
    if (diag.mov_csd == 2) {
      report += "==== mov_probe A：给 csd-0 ====\n";
      report += rlvm_android::RunMovProbe(diag.mov_probe, mov_out_dir,
                                          diag.mov_codec, true);
      report += "==== mov_probe B：不给 csd-0 ====\n";
      report += rlvm_android::RunMovProbe(diag.mov_probe, mov_out_dir,
                                          diag.mov_codec, false);
    } else {
      report += rlvm_android::RunMovProbe(diag.mov_probe, mov_out_dir,
                                          diag.mov_codec, diag.mov_csd != 0);
    }
  }
  // v0.2.3 影片探针（M3a 对照）：让**平台**去解复用 MPEG-PS。
  if (!diag.mov_probe_path.empty()) {
    std::string mov_out_dir;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      mov_out_dir = g_diag_dir;
    }
    std::string mov_path = diag.mov_probe_path;
    // 允许逗号分隔多个文件，一次引擎启动把 A/B/C 都跑出来。
    size_t from = 0;
    while (from <= mov_path.size()) {
      const size_t comma = mov_path.find(',', from);
      std::string one = mov_path.substr(
          from, comma == std::string::npos ? std::string::npos : comma - from);
      if (!one.empty()) {
        if (one.find('/') == std::string::npos && !mov_out_dir.empty()) {
          one = mov_out_dir + "/" + one;  // 只给文件名 = 诊断目录下的文件
        }
        report += "==== mov_extract: " + one + " ====\n";
        report += rlvm_android::RunMovExtractProbe(one, mov_out_dir);
      }
      if (comma == std::string::npos) break;
      from = comma + 1;
    }
  }

  // v0.2.3 / M3b 影片上屏（自测开关）：引擎启动时直接起播一部影片，方便在没有
  // 脚本触发点的游戏里肉眼验证（KW 的脚本只调 movPlayEx，触发点在 OP 那一段）。
  if (!diag.mov_test.empty()) {
    std::string mov_id = diag.mov_test;
    if (mov_id.find('/') == std::string::npos) {
      mov_id = "MOV/" + mov_id + ".mpg";
    }
    // 自测不受脚本控制：真实 OP 场景里脚本本来会先 bgmStop，这里替它做掉，
    // 否则游戏 BGM 会和影片声音叠在一起（真机反馈过）。
    system.sound().BgmStop();
    system.sound().WavStopAll();
    const bool mov_ok = rlvm_android::MovPlayer::Instance().Play(
        mov_id, 0, 0, 799, 599, diag.mov_test_ms, diag.mov_test_loop);
    report += std::string("mov_test: ") + (mov_ok ? "起播 " : "打不开 ") +
              mov_id + "\n";
    rlvm_android::AppendAppLogLine(std::string("mov_test: ") +
                                   (mov_ok ? "起播 " : "打不开 ") + mov_id);
  }
  // D-033 自检：编码链逐步诊断（enc_selftest=1）。一次运行就能定位是哪一步断了。
  if (diag.enc_selftest) {
    const std::string selftest = rlvm_android::RunEncodingSelfTest();
    report += selftest;
    rlvm_android::AppendAppLogLine(selftest);
  }
  if (diag.font_selftest) {
    const std::string selftest = rlvm_android::RunFontSelfTest();
    report += selftest;
    rlvm_android::AppendAppLogLine(selftest);
  }
  rlvm_android::SetFontProbeEnabled(diag.font_probe);
  if (diag.max_instructions > 0) max_instructions = diag.max_instructions;
  if (diag.trace) machine.set_tracing_on();

  // REGNAME 是 CP932 编码的游戏数据，显示前先转成 UTF-8——
  // 否则报告本身含有非法 UTF-8 字节。
  report += "engine assembled (regname=\"" +
            ToDisplayUtf8(gameexe("REGNAME").ToString("")) + "\")\n";
  report += "diagnostics: trace=" + std::string(diag.trace ? "on" : "off") +
            " blit_stats=" + std::string(diag.blit_stats ? "on" : "off") +
            " blit_fast=" + std::string(diag.blit_fast ? "on" : "off") +
            " dirty_gate=" + std::string(diag.dirty_gate ? "on" : "off") +
            " time_budget_ms=" + std::to_string(diag.time_budget_ms) +
            " max_instructions=" + std::to_string(max_instructions) +
            " frame_log_every=" + std::to_string(diag.frame_log_every) + "\n";

  // 重采样自检：把「音调是否偏高」变成日志里的一个频率数字。
  if (diag.audio_selftest) report += rlvm_android::ResamplerSelfTest();

  // 汉化支持：导出各场景日文文本串（见 docs/LOCALIZATION.md）。
  if (diag.export_jp_text) {
    std::string export_dir;
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      export_dir = g_diag_dir;
    }
    ExportSceneText(archive, export_dir, report);
  }
  LogMemory("after export");

  AndroidGraphicsSystem* graphics =
      dynamic_cast<AndroidGraphicsSystem*>(&system.graphics());

  // 与上游 RLVMInstance::Run 相同的结构：每轮先让子系统跑一遍（含合成一帧），
  // 再以 10ms 为时间片连续执行字节码。
  const unsigned int time_budget_ms = static_cast<unsigned int>(diag.time_budget_ms);
  const unsigned int started = system.event().GetTicks();
  // 只统计本次运行造成的合成量，先清零。
  TakeGraphicsBlitStats();
  g_stop_requested.store(false);
  int executed = 0;
  int frames_presented = 0;
  // global memory 的定期落盘兜底（见下面循环里的说明）。
  unsigned int last_global_flush = system.event().GetTicks();
  std::string stop_reason = "instruction budget exhausted";

  while (executed < max_instructions) {
    if (g_stop_requested.load()) {
      stop_reason = "stop requested";
      break;
    }
    // 挂起（黑屏/应用进后台，见 SetEngineSuspended）：完全不推进——不跑 system.Run
    // （不合成新帧）、不执行字节码，只让出 CPU 等唤醒。音频侧同时静音并暂停回调。
    if (g_engine_suspended.load()) {
      // 挂起（黑屏/进后台）时把 global memory 落盘（v0.2.3 / M4 顺带）：
      // 游戏用来标记「槽位已占用」的 intG[1050+槽] 与 Config 都在 global memory 里，
      // 而它原本只在**引擎正常停止**时写盘——从最近任务直接杀掉进程就丢，LOAD 列表
      // 会列不出来（任务日志.md §16）。这里复用挂起通道，在引擎线程落一次盘。
      if (!g_suspend_flushed.exchange(true)) {
        try {
          Serialization::saveGlobalMemory(machine);
          rlvm_android::AppendAppLogLine("suspend: global memory 已落盘");
        } catch (const std::exception& e) {
          rlvm_android::AppendAppLogLine(
              std::string("suspend: global memory 落盘失败：") + e.what());
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    g_suspend_flushed.store(false);  // 恢复运行 → 下次挂起再落一次
    if (machine.halted()) {
      stop_reason = "machine halted";
      break;
    }
    // time_budget_ms == 0 表示不限时：应用要能一直停在标题/正文上。
    if (time_budget_ms > 0 &&
        system.event().GetTicks() - started > time_budget_ms) {
      stop_reason = "time budget exhausted";
      break;
    }

    system.Run(machine);
    if (graphics != nullptr) {
      CaptureFrame(*graphics);
      ++frames_presented;
    }

    // 进度日志：定位「跑很久但没有输出」这类问题。
    if (frames_presented % 60 == 0) {
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "progress: frames=%d instructions=%d elapsed=%ums rss=%ldkB",
                          frames_presented, executed,
                          system.event().GetTicks() - started, CurrentRssKb());
      // 合成成本（D-022 探针）：每 60 帧（≈1 秒）打一次 —— 回答「卡在哪一层」。
      // 计数一直在维护（见 android_graphics.cpp 的 g_blit_*），但此前**没有任何消费者**，
      // 所以从没被量过。用 diag 的 blit_cost=1 打开（默认关，避免刷日志）。
      if (g_blit_cost_log) {
        uint64_t bcalls = 0, bpixels = 0, btime_us = 0, bworst_us = 0;
        TakeBlitCostSummary(bcalls, bpixels, btime_us);
        const std::string worst = TakeBlitWorstSummary(bworst_us);
        __android_log_print(
            ANDROID_LOG_INFO, kLogTag,
            "blit: calls=%llu pixels=%llu time=%.1fms worst=%.1fms %s",
            (unsigned long long)bcalls, (unsigned long long)bpixels,
            btime_us / 1000.0, bworst_us / 1000.0, worst.c_str());
      }
      // 自我保护：真机上出现过内存暴涨把整机拖垮（系统连带杀掉别的应用）。
      // 宁可让引擎自己停下来，也不要让系统去杀。
      if (CurrentRssKb() > kMemoryGuardKb) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "memory guard: rss=%ld kB > %ld kB, stopping engine",
                            CurrentRssKb(), kMemoryGuardKb);
        stop_reason = "memory guard";
        break;
      }
    }

    // 音频在运行期的周期性证据：active_channels>0 表示游戏自己点的 BGM 还在播；
    // peak 取「本窗口内」的最大值，所以每报一次就清零，避免看成一个累计数。
    if (frames_presented % 300 == 0) {
      rlvm_android::AudioEngine& audio = rlvm_android::AudioEngine::Instance();
      const rlvm_android::AudioEngine::Stats audio_stats = audio.GetStats();
      __android_log_print(ANDROID_LOG_INFO, "rlvm-audio",
                          "runtime: active_channels=%d peak_in_window=%d frames=%llu",
                          audio_stats.active_channels, audio_stats.peak_amplitude,
                          audio_stats.frames_rendered);
      audio.ResetPeak();
    }


    // 上游在遇到长操作时只跳出**内层**时间片（把控制权让给这一帧），
    // 外层循环继续推进——长操作本身由后续的 ExecuteNextInstruction 轮询。
    // 早先这里直接 break 整个循环，导致游戏停在第一个长操作上不再前进。
    // intD 写入扫描（diag: intd_poke=1）：按住方向键时，往 intd_poke_lo..hi
    // 里轮流写 intd_poke_value，每个槽位停留 intd_poke_hold_ms。
    // 把当前写的 (槽位,值) 打进日志；用户只要盯游戏画面"角色动没动"。
    if (diag.intd_poke || diag.intd_dir_poke || diag.intd_hit_poke ||
        diag.intd_right_poke) {
      // 右键（长按松开）→ intD[102] 短脉冲（intd_right_poke，默认开）。
      // 事件层把"长按松开"记成 button2_state_ = 2（1=按住 / 2=抬起）；这里只在
      // 上升沿发 pulse_frames 帧的脉冲，脚本 SEEN7800 看到 `intD[102] == 2`
      // 就会 farcall(7800, 1) 去建暂停菜单（221 号对象 PT_RMENU_*）。
      if (diag.intd_right_poke) {
        AndroidEventSystem& res =
            static_cast<AndroidEventSystem&>(machine.system().event());
        Point rp;
        int rb1 = 0, rb2 = 0;
        res.GetCursorPos(rp, rb1, rb2);
        static int last_rb2 = 0;
        static int right_pulse = 0;
        if (rb2 == 2 && last_rb2 != 2) {
          right_pulse = diag.intd_right_pulse_frames;
        }
        last_rb2 = rb2;
        if (right_pulse > 0) {
          --right_pulse;
          machine.SetIntValue(
              libreallive::IntMemRef(libreallive::INTD_LOCATION,
                                     diag.intd_right_slot),
              diag.intd_right_value);
          if (diag.input_trace) {
            rlvm_android::AppendAppLogLine(
                "[input] right_poke: intD[" +
                std::to_string(diag.intd_right_slot) + "] = " +
                std::to_string(diag.intd_right_value));
          }
        } else {
          machine.SetIntValue(
              libreallive::IntMemRef(libreallive::INTD_LOCATION,
                                     diag.intd_right_slot),
              0);
        }
      }
      // ── 击打（鼠标左键） → intD[intd_hit_slot] 直连（diag: intd_hit_poke=1）──
      if (diag.intd_hit_poke) {
        static int last_hit = -1;
        AndroidEventSystem& hes =
            static_cast<AndroidEventSystem&>(machine.system().event());
        const int held = hes.IsMouseButtonHeld(1) ? 1 : 0;
        if (held) {
          machine.SetIntValue(
              libreallive::IntMemRef(libreallive::INTD_LOCATION,
                                     diag.intd_hit_slot),
              1);
          if (last_hit != 1) {
            last_hit = 1;
            if (diag.input_trace) {
              rlvm_android::AppendAppLogLine(
                  "[input] hit_poke: intD[" + std::to_string(diag.intd_hit_slot) +
                  "] = 1");
            }
          }
        } else if (last_hit == 1) {
          last_hit = 0;
          machine.SetIntValue(
              libreallive::IntMemRef(libreallive::INTD_LOCATION,
                                     diag.intd_hit_slot),
              0);
          if (diag.input_trace) {
            rlvm_android::AppendAppLogLine(
                "[input] hit_poke: intD[" + std::to_string(diag.intd_hit_slot) +
                "] = 0");
          }
        }
      }
      // ── pad 方向键 → intD 标志位直连（diag: intd_dir_poke=1）──────────────
      // 按住某方向就把对应槽位写 1，松开写回 0；只在电平**变化**的那一帧写，
      // 避免每帧覆盖（游戏自己也会写这些槽）。
      if (diag.intd_dir_poke) {
        static int last_up = -1, last_down = -1, last_left = -1, last_right = -1;
        struct DirMap {
          int code;
          int slot;
          int* last;
        } map4[4] = {
            {273, diag.intd_dir_up, &last_up},
            {274, diag.intd_dir_down, &last_down},
            {276, diag.intd_dir_left, &last_left},
            {275, diag.intd_dir_right, &last_right},
        };
        AndroidEventSystem& des =
            static_cast<AndroidEventSystem&>(machine.system().event());
        for (auto& e : map4) {
          const int held = des.IsKeyHeld(e.code) ? 1 : 0;
          if (held) {
            // 按住时**每帧重申** 1：与原生引擎一致（游戏/脚本随时可能写 0，
            // 只在边沿写一次会让按住状态被吞掉）。
            machine.SetIntValue(
                libreallive::IntMemRef(libreallive::INTD_LOCATION, e.slot),
                1);
            if (*e.last != 1) {
              *e.last = 1;
              if (diag.input_trace) {
                rlvm_android::AppendAppLogLine(
                    "[input] dir_poke: intD[" + std::to_string(e.slot) + "] = 1");
              }
            }
          } else if (*e.last == 1) {
            *e.last = 0;
            machine.SetIntValue(
                libreallive::IntMemRef(libreallive::INTD_LOCATION, e.slot), 0);
            if (diag.input_trace) {
              rlvm_android::AppendAppLogLine(
                  "[input] dir_poke: intD[" + std::to_string(e.slot) + "] = 0");
            }
          }
        }
      }
      AndroidEventSystem& pes =
          static_cast<AndroidEventSystem&>(machine.system().event());
      const bool dir_held = pes.IsKeyHeld(273) || pes.IsKeyHeld(274) ||
                            pes.IsKeyHeld(276) || pes.IsKeyHeld(275);
      // intd_dir_poke 模式下只跑"方向→槽位"直连，不跑轮换扫描。
      if (!diag.intd_dir_poke && (dir_held || diag.intd_poke_free)) {
        static unsigned int last_advance = 0;
        static int cur_slot = INT_MIN;
        const unsigned int now_ms = system.event().GetTicks();
        if (cur_slot == INT_MIN) cur_slot = diag.intd_poke_lo;
        if (last_advance == 0) last_advance = now_ms;
        if (now_ms - last_advance >=
            static_cast<unsigned int>(diag.intd_poke_hold_ms)) {
          last_advance = now_ms;
          cur_slot = (cur_slot >= diag.intd_poke_hi) ? diag.intd_poke_lo
                                                     : cur_slot + 1;
        }
        machine.SetIntValue(
            libreallive::IntMemRef(libreallive::INTD_LOCATION, cur_slot),
            diag.intd_poke_value);
        static int logged_slot = INT_MIN;
        if (logged_slot != cur_slot) {
          logged_slot = cur_slot;
          rlvm_android::AppendAppLogLine(
              "[input] intd_poke: 正在写 intD[" + std::to_string(cur_slot) +
              "] = " + std::to_string(diag.intd_poke_value));
        }
      }
    }

    const unsigned int slice_start = system.event().GetTicks();
    unsigned int now = slice_start;
    do {
      machine.ExecuteNextInstruction();
      ++executed;
      now = system.event().GetTicks();
    } while (!machine.CurrentLongOperation() && !system.force_wait() &&
             (now - slice_start < 10));
    system.set_force_wait(false);

    // 渲染树导出（面板「导出渲染树」按钮）：只在引擎线程里读 graphics，导出一次。
    if (graphics != nullptr && g_dump_tree_request.exchange(false)) {
      std::ostringstream tree;
      graphics->Refresh(&tree);
      rlvm_android::AppendAppLogLine("graphics tree dump（按需导出）:\n" + tree.str());
      __android_log_print(ANDROID_LOG_INFO, kLogTag, "graphics tree dumped on request");
    }

    // 输入取证（diag: input_trace=1）：每帧只在**内容变化**时打一行
    //   [input] keys=UDLR … cursor=x,y mouse=b1,b2 intD95_115=…
    // 用途：区分「按键没到达引擎」和「引擎收到了但没写进 intD」
    // （小游戏方向键不通的两种根因，见 docs/INPUT-KEY-RECON.md）。
    if (diag.input_trace) {
      AndroidEventSystem& es =
          static_cast<AndroidEventSystem&>(machine.system().event());
      Point cur;
      int mb1 = 0, mb2 = 0;
      es.GetCursorPos(cur, mb1, mb2);
      std::ostringstream line;
      line << "[input] keys=";
      line << (es.IsKeyHeld(273) ? 'U' : '-');   // RLKEY_UP
      line << (es.IsKeyHeld(274) ? 'D' : '-');   // RLKEY_DOWN
      line << (es.IsKeyHeld(276) ? 'L' : '-');   // RLKEY_LEFT
      line << (es.IsKeyHeld(275) ? 'R' : '-');   // RLKEY_RIGHT
      line << " ctrl=" << (es.IsKeyHeld(306) ? 1 : 0)
           << " ret=" << (es.IsKeyHeld(13) ? 1 : 0)
           << " mouse=" << mb1 << "," << mb2 << " cursor=" << cur.x() << ","
           << cur.y() << " intD95_115=";
      for (int i = 95; i <= 115; ++i) {
        line << machine.GetIntValue(
            libreallive::IntMemRef(libreallive::INTD_LOCATION, i));
        if (i != 115) line << ',';
      }
      const std::string s = line.str();
      if (s != g_last_input_line) {
        g_last_input_line = s;
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", s.c_str());
      }
    }

    // 对象活体时间线（diag: wipe_log=1）：把 195..255 号对象的
    // [可见/有数据/WipeCopy] 压成一行，**只在变化时**输出。
    // 意义：不用手按「导出渲染树」也能看到地图(#201)是哪一帧、以什么状态消失的；
    // 配合 ClearAndPromoteObjects 的 [wipe] 明细就能区分
    // 「被擦除」还是「被 bg 槽位覆盖」。
    if (graphics != nullptr && rlvm_android::LbWipeLogEnabled()) {
      const int dig_lo = 195, dig_hi = 255;
      std::ostringstream digest;
      LazyArray<GraphicsObject>& objs = graphics->GetForegroundObjects();
      for (AllocatedLazyArrayIterator<GraphicsObject> it = objs.begin(),
                                                    e = objs.end();
           it != e; ++it) {
        const int pos = static_cast<int>(it.pos());
        if (pos < dig_lo || pos > dig_hi) continue;
        digest << ' ' << pos << ':' << (it->visible() ? 'v' : '-')
               << (it->has_object_data() ? 'd' : '-') << 'w'
               << it->wipe_copy();
      }
      const std::string d = digest.str();
      if (d != g_last_obj_digest) {
        g_last_obj_digest = d;
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "objs[%d..%d]:%s", dig_lo,
                            dig_hi, d.c_str());
      }
    }

    // global memory 的**兜底**落盘（60 秒一次）：挂起那一笔是主要保障，这一笔防的是
    // 「没走到挂起就被杀」——最多丢 60 秒内的槽位标记/Config 改动。17KB 的写入，
    // 开销可以忽略。
    if (system.event().GetTicks() - last_global_flush >= 60000) {
      try {
        Serialization::saveGlobalMemory(machine);
      } catch (const std::exception&) {
        // 落盘失败不打断游戏（路径不可写时挂起那一笔也会打日志）。
      }
      last_global_flush = system.event().GetTicks();
    }
  }

  // 影片：引擎循环结束就收摊（停解码线程、释放解码器与 fd）。
  rlvm_android::MovPlayer::Instance().Stop();
  report += "mov frames decoded = " +
            std::to_string(rlvm_android::MovPlayer::Instance().decoded_frames()) +
            " dropped = " +
            std::to_string(rlvm_android::MovPlayer::Instance().dropped_frames()) +
            "\n";

  report += "instructions executed = " + std::to_string(executed) + "\n";
  report += "frames presented = " + std::to_string(frames_presented) + "\n";
  report += "stop reason = " + stop_reason + "\n";
  report += "halted = " + std::string(machine.halted() ? "yes" : "no") + "\n";
  const GraphicsBlitStats blits = TakeGraphicsBlitStats();
  if (diag.blit_stats) {
    report += "graphics blits: calls=" + std::to_string(blits.calls) +
              " written_pixels=" + std::to_string(blits.written_pixels) +
              " nonblack_pixels=" + std::to_string(blits.nonblack_pixels) + "\n";
  }

  // 图形栈转储：上游的 GraphicsSystem::Refresh(ostream*) 会把每个对象渲染时的
  // src/dst 矩形、alpha、可见性一并打印出来。这是判断「对象没被画」与
  // 「对象画到了屏幕外/全透明」最直接的手段。
  if (diag.dump_graphics && graphics != nullptr) {
    std::ostringstream tree;
    graphics->Refresh(&tree);
    report += "graphics tree dump:\n" + tree.str();
  }

  // 注意：这里**不再**主动播 BGM01。那段脚手架验证代码会在游戏自己
  // 请求标题曲之后抢走通道，再 BgmStop() 把游戏音乐停掉——真机上表现就是
  //「开头能听到一点，随后被测试音乐打断，然后彻底没声音」。
  // 现在音频完全由游戏脚本驱动（日志里可见 play channel=30 file=BGM/BGM14.nwa）。
  // 音频链路的证据改为运行期周期上报：见下方 progress 日志里的 audio 行。

  // 与上游 RLVMInstance 的收尾一致：把全局数据写回（Config 等）。
  // 写在 RunEngineOn 退出前，也就是用户点「停止引擎」或时间片用尽时。
  try {
    Serialization::saveGlobalMemory(machine);
    report += "global memory saved\n";
  } catch (const std::exception& e) {
    report += std::string("global memory save failed: ") + e.what() + "\n";
  }
}

jstring RunScenario(JNIEnv* env, jobject /*thiz*/, jstring jdir,
                    jint max_instructions) {
  namespace fs = boost::filesystem;
  std::string report;

  // 默认不限时运行，重复点击会起第二台引擎；这里拒绝并存。
  RunGuard guard;
  if (!guard.acquired()) {
    report += "ERROR: 已有一台引擎在运行，请先点「停止引擎」。\n";
    LogReport(kLogTag, "run report", report);
    return NewSafeJavaString(env, report);
  }

  try {
    const std::string dir = JStringToUtf8(env, jdir);
    const fs::path root(dir);
    const fs::path gameexe_path = CorrectPathCase(root / "Gameexe.ini");
    const fs::path seen_path = CorrectPathCase(root / "Seen.txt");

    if (gameexe_path.empty() || seen_path.empty()) {
      report += "ERROR: Gameexe.ini or Seen.txt missing "
                "(case-corrected lookup also failed)\n";
      return NewSafeJavaString(env, report);
    }

    Gameexe gameexe(gameexe_path);
    // 上游 RLVMInstance 会写入 __GAMEPATH；资源查找层需要它。
    gameexe("__GAMEPATH") = dir;
    {
      std::string gameexe_text;
      std::ifstream gameexe_in(gameexe_path.string(), std::ios::binary);
      if (gameexe_in) {
        std::ostringstream buffer;
        buffer << gameexe_in.rdbuf();
        gameexe_text = buffer.str();
      }
      AddMissingWindowDefaults(gameexe, gameexe_text);
    }
    rlvm_android::SetGameFileSystem(
        rlvm_android::MakePosixGameFileSystem(dir));
    // 注意：这里**不**把存档指到游戏目录（SAF 侧）——存档菜单除了按槽位查
    // 存在性，还会用 boost 枚举整个存档目录，那条路径尚未接到 GameFileSystem，
    // 会出现"存得下但不显示"。指到普通路径（应用内）时四处都成立，故沿用默认。
    SetGameSaveDirectoryOverride(boost::filesystem::path());

    libreallive::Archive archive(seen_path.string(),
                                 gameexe("REGNAME").ToString(""));
    AndroidSystem system(gameexe);
    RunEngineOn(system, gameexe, archive, max_instructions, report);
    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "run report", report);
  return NewSafeJavaString(env, report);
}

/**
 * SAF 版本：全部文件都经由用户在系统选择器中授权的目录树访问，
 * 不拼接任何 File 路径（task.md 硬性要求）。
 *
 * Gameexe.ini 很小，整体读入后走 istream 构造；
 * SEEN.TXT 走 fd + mmap，避免把整个场景归档复制一遍。
 */
jstring RunScenarioSaf(JNIEnv* env, jobject /*thiz*/, jint max_instructions) {
  std::string report;

  // 同上：先拿到唯一运行权，再动 SAF 后端与引擎。
  RunGuard guard;
  if (!guard.acquired()) {
    report += "ERROR: 已有一台引擎在运行，请先点「停止引擎」。\n";
    LogReport(kLogTag, "saf run report", report);
    return NewSafeJavaString(env, report);
  }

  try {
    std::shared_ptr<rlvm_android::SafBackend> backend = rlvm_android::GetSafBackend();
    if (!backend) {
      report += "ERROR: no SAF backend installed\n";
      return NewSafeJavaString(env, report);
    }

    report += "SAF root listing:\n";
    std::vector<std::string> root_names;
    for (const auto& entry : backend->ListDirectory("")) {
      report += "  " + entry.name + (entry.is_directory ? "/" : "") + "\n";
      root_names.push_back(entry.name);
    }

    std::string gameexe_text;
    if (!rlvm_android::SafReadAll("Gameexe.ini", gameexe_text)) {
      report += "ERROR: cannot read Gameexe.ini via SAF\n";
      return NewSafeJavaString(env, report);
    }
    report += "Gameexe.ini read via SAF: " +
              std::to_string(gameexe_text.size()) + " bytes\n";
    std::istringstream gameexe_stream(gameexe_text);
    Gameexe gameexe(gameexe_stream);
    // SAF 下没有真实路径；__GAMEPATH 只在退化为普通路径后端时才会被使用。
    gameexe("__GAMEPATH") = std::string("saf:/");
    AddMissingWindowDefaults(gameexe, gameexe_text);
    rlvm_android::SetGameFileSystem(rlvm_android::MakeSafGameFileSystem());
    // 同上：SAF 下暂不指定覆盖，继续用 $HOME/.rlvm/<REGNAME>/（真实路径，
    // 写入/读取/槽位检查/目录枚举四处都能工作）。
    SetGameSaveDirectoryOverride(boost::filesystem::path());
    AndroidSystem system(gameexe);

    // 资源查找层探针：走上游 System::FindFile 的完整链路
    //（读 #FOLDNAME -> 枚举目录 -> 扩展名匹配 -> 生成文件标识）。
    // 目标文件 g00/doesntmatter.g00 来自上游自带测试数据 test/Gameroot。
    {
      const boost::filesystem::path found =
          system.FindFile("doesntmatter", std::vector<std::string>{"g00"});
      if (found.empty()) {
        report += "FindFile(doesntmatter, g00) -> <not found>\n";
      } else {
        report += "FindFile(doesntmatter, g00) -> \"" + found.string() + "\"\n";
        std::shared_ptr<rlvm_android::GameFileSystem> vfs =
            rlvm_android::GetGameFileSystem();
        const int lookup_fd = vfs ? vfs->OpenFd(found.string()) : -1;
        report += std::string("  open via file system -> ") +
                  (lookup_fd >= 0 ? "fd=" + std::to_string(lookup_fd) : "FAILED") +
                  "\n";
        if (lookup_fd >= 0) close(lookup_fd);
      }
    }

    // 文本容器选择：默认游戏目录里的 Seen.txt；开关打开时改用用户指定的那个文件
    // （合并了汉化的容器）。指定的文件打不开就回退，避免"开关一开游戏直接起不来"。
    std::string container_path;
    {
      std::lock_guard<std::mutex> lock(g_text_container_mutex);
      container_path = g_text_container_path;
    }
    std::string container_label = "Seen.txt (游戏目录)";
    int fd = -1;
    if (!container_path.empty()) {
      fd = backend->OpenFd(container_path);
      if (fd >= 0) {
        container_label = container_path;
        report += "text container \"" + container_path +
                  "\" opened via SAF fd=" + std::to_string(fd) + "\n";
      } else {
        report += "WARNING: 指定容器 " + container_path +
                  " 打不开，回退到游戏目录 Seen.txt\n";
      }
    }
    if (fd < 0) {
      fd = backend->OpenFd("Seen.txt");
      if (fd < 0) {
        report += "ERROR: cannot open Seen.txt via SAF\n";
        return NewSafeJavaString(env, report);
      }
      report += "Seen.txt opened via SAF fd=" + std::to_string(fd) + "\n";
    }

    {
      // Archive 内部完成 mmap，之后即可关闭 fd（映射仍然有效）。
      libreallive::Archive archive(fd, "saf:/" + container_label,
                                   gameexe("REGNAME").ToString(""));
      close(fd);

      // 补丁机制：SEEN####.TXT 场景覆盖。
      // SAF 下没有可供 boost::filesystem 枚举的目录，因此这里用 SAF 后端
      // 列出文件名，再按名打开——文件名规则本身仍由 Archive 判定。
      const std::vector<std::string>& names = root_names;
      archive.ApplyOverrides(
          names, [&backend](const std::string& name) -> libreallive::Mapping* {
            const int override_fd = backend->OpenFd(name);
            if (override_fd < 0) return nullptr;
            libreallive::Mapping* mapping = nullptr;
            try {
              mapping = new libreallive::Mapping(override_fd, 0);
            } catch (...) {
              mapping = nullptr;
            }
            close(override_fd);
            return mapping;
          });

      int scenarios = 0;
      std::string indices;
      for (libreallive::Archive::const_iterator it = archive.begin();
           it != archive.end(); ++it) {
        ++scenarios;
        // 只列前 16 个：报告要逐行打印，索引全列会让开头几行淹没在噪声里。
        if (scenarios <= 16) {
          if (!indices.empty()) indices += ",";
          indices += std::to_string(it->first);
        }
      }
      if (scenarios > 16) indices += ",...";
      report += "Seen via SAF: TOC entries=" + std::to_string(scenarios) +
                "; indices=[" + indices + "]\n";
      // 诊断：引擎拿来解码文本的编码。0=CP932 1=CP936(GBK) 2=CP1252 3=CP949。
      report += "text encoding: probable=" +
                std::to_string(archive.GetProbableEncodingType()) + "\n";

      RunEngineOn(system, gameexe, archive, max_instructions, report);
    }

    report += "RUN OK\n";
  } catch (const std::exception& e) {
    report += std::string("EXCEPTION: ") + e.what() + "\n";
  } catch (...) {
    report += "EXCEPTION: unknown\n";
  }

  LogReport(kLogTag, "saf run report", report);
  return NewSafeJavaString(env, report);
}

/** 由 Kotlin 侧在取得 SAF 目录授权后调用，安装 SAF 后端。 */
void SetSafBackendFromJava(JNIEnv* env, jobject /*thiz*/, jobject backend) {
  rlvm_android::InstallJniSafBackend(env, backend);
}

/**
 * 告知 native 侧诊断文件的所在目录（应用的外部文件目录）。
 *
 * 真机迭代一轮要重新构建 + 安装，很慢；把「跑多久 / 是否逐条追踪」这类
 * 观察参数放进目录下的 rlvm-diag.txt，用 adb push 改一行即可换一次实验。
 */
void SetDiagnosticsDir(JNIEnv* env, jobject /*thiz*/, jstring jdir) {
  std::lock_guard<std::mutex> lock(g_diag_mutex);
  g_diag_dir = JStringToUtf8(env, jdir);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "diagnostics dir = %s",
                      g_diag_dir.c_str());
  // native 侧的关键日志也落一份到应用日志文件（用户看不到 logcat）。
  rlvm_android::SetAppLogFile(g_diag_dir + "/rlvm-log.txt");
  // 上游 System::GetHomeDirectory() 依赖 HOME/HOMEDRIVE/USERPROFILE，而 Android
  // 一个都没有——缺它会在"计算存档目录"时抛
  // "Could not find location of home directory."，存档与 Config 全部失败。
  // 这里给进程补一个可写的 HOME（应用外部文件目录），存档即可持久保存，
  // 且用户能在文件管理器里看到。
  if (!g_diag_dir.empty()) {
    setenv("HOME", g_diag_dir.c_str(), 1);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "HOME=%s", g_diag_dir.c_str());
  }
}

/**
 * 请求停止当前正在运行的引擎。
 *
 * 应用默认不限时运行（要能一直停在标题/正文上），所以必须有一个从 UI 喊停的通道：
 * 只置一个标志，引擎线程在下一轮循环开头看到后正常收尾（走完报告流程）。
 */
void RequestStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  g_stop_requested.store(true);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "stop requested");
}

/**
 * 挂起 / 恢复引擎（屏幕熄灭、应用进后台）。
 *
 * 与 requestStop 的区别：停止是「收尾退出」，挂起是「原地冻结」——进度、存档、
 * 音频位置全都不动，唤醒后接着跑。这里只置标志，真正的收敛点在引擎循环开头
 * （不推进指令、不合成帧）与 AudioEngine::SetSuspended（静音 + 停 AAudio 回调）。
 */
void SetEngineSuspended(JNIEnv* /*env*/, jobject /*thiz*/, jboolean suspended) {
  const bool value = suspended != JNI_FALSE;
  g_engine_suspended.store(value);
  rlvm_android::AudioEngine::Instance().SetSuspended(value);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "engine suspended=%s",
                      value ? "yes" : "no");
}

/**
 * 请求导出当前渲染树（v0.2.4 诊断）：置位后由**引擎线程**在循环里导出一次，
 * 结果写进应用日志（面板「日志」里能看）。比 dump_graphics=1 好用——不用重启引擎，
 * 遇到一次性画面（选项、转场）时当场就能抓。
 */
void RequestGraphicsDump(JNIEnv* /*env*/, jobject /*thiz*/) {
  g_dump_tree_request.store(true);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "graphics dump requested");
}

/**
 * 指定文本容器（汉化/原版的 SEEN 归档）在游戏目录里的相对路径，
 * 空串 = 回到游戏目录的 Seen.txt。
 *
 * 为什么要有这个开关：合并后的汉化容器是**另一个文件**，不能要求用户去覆盖游戏目录里
 * 的 SEEN.TXT（那是用户的原始文件，覆盖了就没法切回日文）。所以 UI 上给一个显式开关 +
 * 应用内文件列表，native 在开引擎时按这个相对路径走 SAF 打开容器；关掉即恢复原版。
 * 注意：游戏目录里的 Seen####.txt 覆盖文件仍然优先于容器（补丁机制的既有语义）。
 */
void SetTextContainerPath(JNIEnv* env, jobject /*thiz*/, jstring jpath) {
  const std::string path = jpath == nullptr ? std::string() : JStringToUtf8(env, jpath);
  {
    std::lock_guard<std::mutex> lock(g_text_container_mutex);
    g_text_container_path = path;
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "text container = %s",
                      path.empty() ? "(game dir Seen.txt)" : path.c_str());
}

/**
 * 触摸/鼠标输入（T2.3）。
 *
 * 坐标已经是**游戏帧坐标**（Kotlin 侧按帧在视图里的实际绘制矩形换算过），
 * 这里只做入队：真正的注入在引擎线程的 ExecuteEventSystem 里完成。
 */
void TouchEvent(JNIEnv* /*env*/, jobject /*thiz*/, jint action, jfloat x, jfloat y,
                jint buttons) {
  AndroidSystem* system = g_current_system.load();
  if (system == nullptr) return;  // 引擎没在跑，忽略

  // 影片播放中点一下 = 跳过（v0.2.3 / M4）。这一下**不转发**给脚本，
  // 否则跳过影片的同时还会顺手推进一句对话。
  if (action == 0 /* TOUCH_DOWN */ && rlvm_android::MovPlayer::Instance().playing()) {
    rlvm_android::MovPlayer::Instance().Stop();
    rlvm_android::AppendAppLogLine("mov: 点击跳过影片");
    return;
  }

  // buttons 为 0 时退回诊断文件的缺省值（1=左键 2=右键 3=两者）。
  const int resolved = buttons != 0 ? static_cast<int>(buttons)
                                    : g_touch_buttons.load();
  static_cast<AndroidEventSystem&>(system->event())
      .PostTouchEvent(static_cast<int>(action),
                      Point(static_cast<int>(x), static_cast<int>(y)),
      resolved);
}

/**
 * 按键事件（v0.2.1 T7.2）。
 *
 * key_code 取 systems/base/event_listener.h 的 RLKEY_*（例如 LSHIFT=304、LCTRL=306、
 * UP=273）。与 TouchEvent 一样只入队，注入发生在引擎线程。
 *
 * 今天 RealLive 脚本还读不到键盘状态（RLVM 没有对应模块），这条通道先服务系统级消费者
 * （Shift/Ctrl）并为将来小游戏的 DLL 模拟预留。
 */
void KeyEvent(JNIEnv* /*env*/, jobject /*thiz*/, jint key_code, jboolean pressed) {
  AndroidSystem* system = g_current_system.load();
  if (system == nullptr) return;  // 引擎没在跑，忽略
  static_cast<AndroidEventSystem&>(system->event())
      .PostKeyEvent(static_cast<int>(key_code), pressed != JNI_FALSE);
}

/**
 * 滚轮事件：log（回想）在原生引擎里就是鼠标滚轮触发的，而 Android 端此前没有任何
 * 滚轮通路 —— 所以 log 打不开。delta > 0 = 上滚（BackPage / 往回翻），< 0 = 下滚。
 * 与 TouchEvent/KeyEvent 一样只入队，注入发生在引擎线程。
 */
void WheelEvent(JNIEnv* /*env*/, jobject /*thiz*/, jint delta) {
  AndroidSystem* system = g_current_system.load();
  if (system == nullptr) return;
  static_cast<AndroidEventSystem&>(system->event())
      .PostWheelEvent(static_cast<int>(delta));
}

/** 当前呈现帧的尺寸：高 16 位为宽、低 16 位为高；暂无帧时返回 0。 */
jint GetFrameSize(JNIEnv* /*env*/, jobject /*thiz*/) {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame_width <= 0 || g_frame_height <= 0) return 0;
  return (g_frame_width << 16) | (g_frame_height & 0xFFFF);
}

/**
 * 把当前帧复制到调用方提供的直接缓冲区（宽*高 个 RGBA8888 像素）。
 * 返回帧序号；缓冲区过小或暂无帧时返回 -1。
 * 序号与上次相同表示没有新帧，GL 线程据此跳过重复上传。
 */
jint CopyFrameToBuffer(JNIEnv* env, jobject /*thiz*/, jobject buffer) {
  void* address = env->GetDirectBufferAddress(buffer);
  if (address == nullptr) return -1;
  const jlong capacity = env->GetDirectBufferCapacity(buffer);

  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame_pixels.empty()) return -1;
  const size_t bytes = g_frame_pixels.size() * sizeof(uint32_t);
  if (capacity < static_cast<jlong>(bytes)) return -1;

  std::memcpy(address, g_frame_pixels.data(), bytes);
  // 诊断：GL 线程每次取帧时统计一次内容哈希。用来区分
  //   「引擎在产新画面、但没上屏（GL/present 的问题）」 vs
  //   「引擎产出的画面本身就是同一张（合成/对象的问题）」。
  {
    static uint64_t n = 0;
    ++n;
    if (n % 60 == 0) {
      uint64_t h = 1469598103934665603ull;
      const uint32_t* p = g_frame_pixels.data();
      const size_t step = 997;  // 抽样步长，避免每帧扫 48 万像素
      for (size_t i = 0; i < g_frame_pixels.size(); i += step) {
        h = (h ^ p[i]) * 1099511628211ull;
      }
      __android_log_print(ANDROID_LOG_INFO, kLogTag,
                          "frame content hash=%016llx serial=%u n=%llu",
                          (unsigned long long)h, g_frame_serial,
                          (unsigned long long)n);
    }
  }
  return static_cast<jint>(g_frame_serial);
}

const JNINativeMethod kNativeMethods[] = {
    {"versionString", "()Ljava/lang/String;", reinterpret_cast<void*>(VersionString)},
    {"probeAbi", "()I", reinterpret_cast<void*>(ProbeAbi)},
    {"probeGameDir", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(ProbeGameDir)},
    {"runScenario", "(Ljava/lang/String;I)Ljava/lang/String;",
     reinterpret_cast<void*>(RunScenario)},
    {"setSafBackend", "(Lorg/rlvm/android/SafFileSystem;)V",
     reinterpret_cast<void*>(SetSafBackendFromJava)},
    {"setDiagnosticsDir", "(Ljava/lang/String;)V",
     reinterpret_cast<void*>(SetDiagnosticsDir)},
    {"requestStop", "()V", reinterpret_cast<void*>(RequestStop)},
    {"setEngineSuspended", "(Z)V", reinterpret_cast<void*>(SetEngineSuspended)},
    {"requestGraphicsDump", "()V", reinterpret_cast<void*>(RequestGraphicsDump)},
    {"setTextContainerPath", "(Ljava/lang/String;)V",
     reinterpret_cast<void*>(SetTextContainerPath)},
    {"touchEvent", "(IFFI)V", reinterpret_cast<void*>(TouchEvent)},
    {"keyEvent", "(IZ)V", reinterpret_cast<void*>(KeyEvent)},
    {"wheelEvent", "(I)V", reinterpret_cast<void*>(WheelEvent)},
    {"runScenarioSaf", "(I)Ljava/lang/String;",
     reinterpret_cast<void*>(RunScenarioSaf)},
    {"getFrameSize", "()I", reinterpret_cast<void*>(GetFrameSize)},
    {"copyFrameToBuffer", "(Ljava/nio/ByteBuffer;)I",
     reinterpret_cast<void*>(CopyFrameToBuffer)},
};

/** 必须与 Kotlin 侧 org.rlvm.android.NativeBridge 完全一致。 */
constexpr char kBridgeClassName[] = "org/rlvm/android/NativeBridge";

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  // 尽早接管 stdout / stderr：上游 RLVM 的诊断信息全走这两个流，
  // 而 Android 应用默认把它们丢进 /dev/null。晚一步装就会漏掉早期输出。
  rlvm_android::InstallLogRedirect();

  // 安装"游戏文件标识 → fd"钩子：语音归档在 SAF 下靠它读文件（见上面说明）。
  SetOpenGameFileFdHook(&OpenGameFileFdHookImpl);
  // 写入变体：存档 / Config 落盘（SAF 下由 Kotlin 侧建文档）。
  SetOpenGameFileWriteFdHook(&OpenGameFileWriteFdHookImpl);

  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass bridge_class = env->FindClass(kBridgeClassName);
  if (bridge_class == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "FindClass(%s) failed", kBridgeClassName);
    return JNI_ERR;
  }

  const jint registered = env->RegisterNatives(
      bridge_class, kNativeMethods,
      static_cast<jint>(sizeof(kNativeMethods) / sizeof(kNativeMethods[0])));
  if (registered != JNI_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "RegisterNatives failed");
    return JNI_ERR;
  }

  // 保存 JavaVM：SAF 后端需要从 native 回调 Kotlin。
  rlvm_android::SetJavaVm(vm);

  __android_log_print(ANDROID_LOG_INFO, kLogTag, "native bridge registered");
  return JNI_VERSION_1_6;
}
