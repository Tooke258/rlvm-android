/* PT00 x86-32 执行器（第一阶段骨架：整数 + 内存/栈 + call/ret）。
 *
 * 目标：不依赖任何现成模拟器（许可证原因，见 docs/LB-MINIGAME-RETRO.md），
 * 用一个最小解释器把原版 PT00.dll 跑起来。范围已经被量死：
 *   可达代码 4897 条指令 / 82 种助记符，**没有 SSE2**，浮点是 x87（约 30 条）。
 *
 * 分三步走（本文件是第一步）：
 *   1) 整数 + 内存/栈 + call/ret + 0f 双字节族        ← 现在这里
 *   2) x87（fld/fst/fadd/fmul/fcom/…）
 *   3) KERNEL32 shim（堆 / TLS / 时间 / rand）
 *
 * 验收方式：同一串调用 + 同一种子，与 tools/pt00_oracle/oracle.exe 的 intD
 * 轨迹逐位比对。**未实现的指令一律打日志并停下**——这份日志就是覆盖率清单。
 *
 * 构建：tools\pt00_emu\build.bat
 * 运行：emu.exe <PT00.dll> [calls.txt] [--seed seed.bin]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h> /* 缺失时 sqrt/sin/cos/pow/atan2/... 会被隐式声明成 int，浮点路径全错 */
#if !defined(_WIN32)
#include <signal.h>   /* 崩溃自证处理器（Android 宿主） */
#include <unwind.h>   /* _Unwind_Backtrace：API 31 上还没有 backtrace() */
#include <unistd.h>   /* write() */
#include <dlfcn.h>    /* dladdr：取本模块加载基址，把崩溃地址变成"模块内偏移" */
#endif

#define GUEST_SIZE (64u * 1024u * 1024u) /* 平坦 32 位空间的一部分，够这个 DLL 用 */
#define IMAGE_BASE 0x10000000u
#define INTD_COUNT 2000
#define INTF_COUNT 2000

/* guest 空间覆盖 [0x10000000, 0x10000000+GUEST_SIZE)——DLL 的镜像基址就在
 * 0x10000000，所以必须做一层基址翻译，不能直接从 0 起算。 */
#define GUEST_BASE 0x10000000u
static uint8_t *g_mem;

/* 运行环境与 trace 开关（放前面：wr32 的写入日志要用到） */
#define SENTINEL 0x30000000u
#define INTD_BASE 0x10400000u
#define INTF_BASE 0x10500000u
#define CTX_BASE 0x10600000u
static int g_trace = 0;
static int g_trace_left = 0;
static uint32_t g_trace_min = 0;
static uint32_t g_insn_addr = 0; /* 当前指令的起始地址（UNIMPL 报点用，cpu.eip-1 会被操作数字节带偏） */
static int g_trace_ctx = 0;      /* 打开后记录对 ctx 块（CTX_BASE..+0x100）与低地址的读写 */
static int g_ctx_log_left = 0;

/* CPU 状态（放前面：wr32 的写入日志要用 cpu.eip） */
typedef struct {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t eip;
  uint32_t eflags;
  uint8_t *fpregs[8];
  int fptop;
} CPU;
static CPU cpu;

/* 导入桩：给每个 KERNEL32 导入函数分一个 guest 地址，写进 IAT。
 * 执行器在 step() 里拦截落在桩区间的 eip，改调宿主实现，然后按 cdecl ret。 */
#define STUB_BASE 0x20000000u
#define STUB_STRIDE 16u
#define STUB_MAX 256
static char g_stub_name[STUB_MAX][64];
static int g_stub_count = 0;
static int g_stub_hits[STUB_MAX];

static uint32_t g_heap_ptr = 0x11400000u; /* CRT 堆的假实现：bump allocator */

/* TIB 影子区：`fs:[x]` 一律映射到 TIB_BASE+x。
 * 关键的是 fs:[0] —— 那是 **SEH 异常链的头指针**。之前我们把它当"永远 0"，
 * 于是 CRT 注册的异常处理器被丢掉，一旦 unwind 走到链头就跳到 0
 * （真机表现：eip=00000000）。fs:[0x18] 要存 TIB 自身地址，CRT 会读它。 */
#define TIB_BASE 0x10300000u
static int g_seg_fs = 0;

static uint32_t guest_alloc(uint32_t n) {
  uint32_t p = g_heap_ptr;
  g_heap_ptr += (n + 15u) & ~15u;
  if (g_heap_ptr >= GUEST_BASE + GUEST_SIZE) {
    static int warned = 0;
    if (!warned) {
      warned = 1;
      fprintf(stderr, "[emu] 假堆用尽（%u 字节已分配）—— 之后的分配会越界\n",
              (unsigned)(g_heap_ptr - 0x11400000u));
    }
  }
  return p;
}

/* 假堆的「指针 -> 大小」表：HeapReAlloc / HeapSize 要用。
 * 以前 HeapReAlloc 直接返回老指针、HeapSize 恒 64 —— DLL 一旦 realloc 变大再往里写，
 * 就会踩掉相邻的来宾数据，最后拿着野指针进死循环（真机运镜时就是死在一个
 * vector 扩容后的拷贝循环里：`mov (%eax),%edx; mov %edx,(%ecx)` 到不了终点）。 */
#define EMU_HEAP_BLOCKS 8192
static struct { uint32_t p, sz; } g_heap_blocks[EMU_HEAP_BLOCKS];
static int g_heap_block_n = 0;

static void heap_note(uint32_t p, uint32_t sz) {
  if (g_heap_block_n < EMU_HEAP_BLOCKS) {
    g_heap_blocks[g_heap_block_n].p = p;
    g_heap_blocks[g_heap_block_n].sz = sz;
    ++g_heap_block_n;
  }
}
static uint32_t heap_size_of(uint32_t p) {
  for (int i = g_heap_block_n - 1; i >= 0; --i)
    if (g_heap_blocks[i].p == p) return g_heap_blocks[i].sz;
  return 0;
}

static inline int in_guest(uint32_t a, uint32_t n) {
  if (a < GUEST_BASE) return 0;
  return (uint64_t)(a - GUEST_BASE) + n <= GUEST_SIZE;
}
static inline uint8_t *gp(uint32_t a) { return g_mem + (a - GUEST_BASE); }

/* 来宾越界访问的兜底：x87 的存取以前是裸指针，DLL 一旦算出野指针就直接 SIGSEGV
 * （真机崩在 pt00_emu_call 里，fault addr 是宿主地址 = g_mem + 巨大偏移）。
 * 这里统一记一笔并返回 NULL，由调用点退化成 0 —— 不崩，才能看出「是哪个 eip、
 * 访问哪个地址」。
 */
static int g_fault_count = 0;
/* 上一次 pt00_emu_call 是否撞到步数上限（= DLL 在内层死循环）。桥那边用它来触发
 * 「最近 N 次 CallDLL」的转储，从而知道是哪个 func 把 DLL 内部状态搞坏的。 */
static int g_last_hit_step_limit = 0;

/* 诊断：监视某个 eip「每次 CallDLL 首次命中」的寄存器 + 结构体现场。
 * 死循环里的 `add $4,%eax` 会把 eax 推到离谱的值（200M 步约 +100MB），事后看
 * 崩溃寄存器根本反推不出循环入口的真实 begin/end，必须靠这个入口现场。 */
static uint32_t g_watch_eip = 0;
static int g_watch_left = 0;
static int g_watch_seen = 0;
static int g_watch_hits = 0;

/* 观察点：eip 命中时记录「每次 CallDLL 第一次命中」的寄存器/结构体现场。
 * 定义放在这里（不受 PT00_EMU_LIBRARY 分支影响），CLI 与 Android 库两边共用。 */
void pt00_emu_set_watch(uint32_t eip, int max_lines) {
  g_watch_eip = eip;
  /* max_lines < 0：只进环形缓冲、不逐条打印（默认用法，日志只留步数上限时的转储）。 */
  g_watch_left = max_lines < 0 ? 0 : (max_lines > 0 ? max_lines : 120);
  g_watch_hits = 0;
  g_watch_seen = 0;
}

static void intd_ring_add(uint32_t idx, uint32_t val, uint32_t eip); /* 定义见下 */

static uint32_t rd32(uint32_t a);   /* 下面定义；guest_fault 里要 dump ctx */

#if !defined(_WIN32)
/* ------------------------------------------------------------------ 崩溃自证
 * 我们踩过两次：真机 tombstone 的 pc/frame 落在纯算术函数里（clang 内联 + 
 * 展开器给的是返回地址），根本看不出到底访问了哪儿。这里自己装 SIGSEGV/SIGBUS
 * 处理器：把「来宾 eip + 寄存器 + 宿址与 g_mem 的偏移」直接写 stderr，然后干净退出。
 * （write() 是 async-signal-safe 的；snprintf 在这里只做只读格式化，实践中可用。）*/
/* 记下被我们替换掉的处理器：bionic/ART 那个才会去叫 debuggerd 生成 tombstone。 */
static struct sigaction g_prev_segv;
static struct sigaction g_prev_bus;
/* 本模块（librlvm.so）的加载基址：崩溃地址减去它，就能直接拿本机未 strip 的
 * librlvm.so 做符号化（否则 ASLR 会让地址对不上）。安装处理器时算一次。 */
static uintptr_t g_host_base = 0;

/* 崩溃时的最小宿主回溯：API 31 上还没有 backtrace()，用 C++ unwinder 取返回地址
 * （不分配内存，只往 fd 2 写十六进制地址），事后用未 strip 的 librlvm.so 符号化。 */
struct emu_bt_state {
  uintptr_t ips[40];
  int n;
};

static _Unwind_Reason_Code emu_bt_cb(struct _Unwind_Context *ctx, void *arg) {
  struct emu_bt_state *st = (struct emu_bt_state *)arg;
  if (st->n >= 40) return _URC_END_OF_STACK;
  uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
  if (ip) st->ips[st->n++] = ip;
  return _URC_NO_REASON;
}

static void emu_bt_dump(void) {
  struct emu_bt_state st;
  st.n = 0;
  _Unwind_Backtrace(emu_bt_cb, &st);
  const char *tag = "[pt00] HOST BT:";
  ssize_t ignored = write(2, tag, strlen(tag));
  (void)ignored;
  for (int i = 0; i < st.n; ++i) {
    char b[24];
    const unsigned long long rel =
        (g_host_base && st.ips[i] >= g_host_base)
            ? (unsigned long long)(st.ips[i] - g_host_base)
            : (unsigned long long)st.ips[i];
    int k = snprintf(b, sizeof(b), " %llx", rel);
    if (k > 0) {
      ignored = write(2, b, (size_t)k);
      (void)ignored;
    }
  }
  ignored = write(2, "\n", 1);
  (void)ignored;
}

static void emu_fault_handler(int sig, siginfo_t *info, void *uc) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
                   "[pt00] HOST FAULT sig=%d addr=%p | g_mem=%p off=%lld | "
                   "eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x "
                   "edi=%08x ebp=%08x esp=%08x\n",
                   sig, info ? info->si_addr : (void *)0, (void *)g_mem,
                   (long long)((char *)(info ? info->si_addr : (void *)0) -
                               (char *)g_mem),
                   cpu.eip, cpu.eax, cpu.ebx, cpu.ecx, cpu.edx, cpu.esi,
                   cpu.edi, cpu.ebp, cpu.esp);
  if (n > 0) {
    ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;
  }
  /* 上面那些寄存器是**来宾**状态，宿主崩（例如空指针）时它们毫无用处 ——
   * 真正要看的是宿主返回地址。 */
  emu_bt_dump();
  /* 1) 优先链回原处理器：bionic/ART 的那个会去叫 debuggerd 生成 tombstone。 */
  {
    const struct sigaction *prev = (sig == SIGSEGV) ? &g_prev_segv : &g_prev_bus;
    if (prev->sa_flags & SA_SIGINFO) {
      if (prev->sa_sigaction) prev->sa_sigaction(sig, info, uc);
    } else if (prev->sa_handler && prev->sa_handler != SIG_DFL &&
               prev->sa_handler != SIG_IGN) {
      prev->sa_handler(sig);
    }
  }
  /* 2) 没人接：先解除屏蔽（处理函数运行期间本信号被自动屏蔽，不解除 raise 只会
   *    排队），再交回默认动作 —— 至少让进程带着正确信号干净地死掉，而不是被
   *    _exit() 伪装成"正常退出"。 */
  {
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);
  }
  signal(sig, SIG_DFL);
  raise(sig);
  _exit(132); /* 兜底 */
}

static void emu_install_fault_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = emu_fault_handler;
  sa.sa_flags = SA_SIGINFO;
  {
    Dl_info di;
    memset(&di, 0, sizeof(di));
    if (dladdr((void *)&emu_install_fault_handler, &di) && di.dli_fbase)
      g_host_base = (uintptr_t)di.dli_fbase;
  }
  sigaction(SIGSEGV, &sa, &g_prev_segv);
  sigaction(SIGBUS, &sa, &g_prev_bus);
}
#else
static void emu_install_fault_handler(void) {}   /* Windows 宿主只用于 CLI */
#endif
/* 最近执行过的来宾 eip（环形）：越界访问时打出来，就能还原「DLL 走到哪一步崩的」 */
#define EIP_RING 64
static uint32_t g_eip_ring[EIP_RING];
static int g_eip_ring_pos = 0;
static void guest_fault(uint32_t a, const char *what) {
  ++g_fault_count;
  if (g_fault_count > 64) return;
  fprintf(stderr, "[pt00] GUEST FAULT %s addr=%08x eip=%08x esp=%08x\n", what, a,
          cpu.eip, cpu.esp);
  if (g_fault_count == 1) {
    /* 第一次越界时把现场摊开：最近 32 条 eip + ctx 块 */
    fprintf(stderr, "[pt00] FAULT eip ring (old->new):");
    for (int i = 0; i < 32; ++i) {
      int p = (g_eip_ring_pos - 32 + i + EIP_RING * 2) % EIP_RING;
      fprintf(stderr, " %08x", g_eip_ring[p]);
    }
    fprintf(stderr, "\n[pt00] FAULT ctx block:");
    for (int i = 0; i < 0x40; i += 4)
      fprintf(stderr, " +%02x=%08x", i, rd32(CTX_BASE + (uint32_t)i));
    fprintf(stderr, "\n");
  }
}
static inline uint8_t *gpc(uint32_t a, unsigned n, const char *what) {
  if (!in_guest(a, n)) {
    guest_fault(a, what);
    return NULL;
  }
  return gp(a);
}

static uint8_t rd8(uint32_t a) { return in_guest(a, 1) ? *gp(a) : 0; }
static uint32_t rd32(uint32_t a) {
  if (g_trace_ctx && g_ctx_log_left > 0 &&
      ((a >= CTX_BASE && a < CTX_BASE + 0x100) || a < 0x1000) &&
      (a & 3) == 0) {
    --g_ctx_log_left;
    fprintf(stderr, "      CTXRD %08x = %08x @%08x\n", a,
            in_guest(a, 4) ? *(uint32_t *)gp(a) : 0, cpu.eip);
  }
  // pt00_trace_ctx=1 时顺带把 **intD 的读数**也记下来：DLL 死循环时，坏掉的
  // 「数量/大小」几乎一定来自它读的某个 intD 槽 —— 把这些槽位和 PC 侧对比即可定位。
  if (g_trace_ctx && a >= INTD_BASE && a < INTD_BASE + 8000 && (a & 3) == 0) {
    uint32_t idx = (a - INTD_BASE) / 4u;
    uint32_t val = in_guest(a, 4) ? *(uint32_t *)gp(a) : 0;
    intd_ring_add(idx, val, cpu.eip);
    if (g_ctx_log_left > 0) {
      --g_ctx_log_left;
      fprintf(stderr, "      INTDRD [%u]=%d @%08x\n", idx, (int)val, cpu.eip);
    }
  }
  return in_guest(a, 4) ? *(uint32_t *)gp(a) : 0;
}
static void wr32(uint32_t a, uint32_t v) {
  if (g_trace && a >= INTD_BASE && a < INTD_BASE + 8000 && (a & 3) == 0) {
    fprintf(stderr, "      WRITE intD[%u]=%d @%08x\n", (a - INTD_BASE) / 4, (int)v,
            cpu.eip);
  }
  /* ctx 取证：DLL 要拿引擎的其它数组（intG/intL/…）一定会先从这个块里取指针，
     所以把 ctx+0x00..0xFF 与低地址的访问记下来就能看出它想要什么。 */
  if (g_trace_ctx && g_ctx_log_left > 0 &&
      ((a >= CTX_BASE && a < CTX_BASE + 0x100) || a < 0x1000) &&
      (a & 3) == 0) {
    --g_ctx_log_left;
    fprintf(stderr, "      CTXWR %08x = %08x @%08x\n", a, v, cpu.eip);
  }
  if (in_guest(a, 4)) *(uint32_t *)gp(a) = v;
}
static void wr8(uint32_t a, uint8_t v) {
  if (in_guest(a, 1)) *gp(a) = v;
}

/* ------------------------------------------------------------------ CPU */
enum { CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7,
       DF = 1u << 10, OF = 1u << 11 };

static inline void set_szp32(uint32_t v) {
  cpu.eflags &= ~(ZF | SF | PF);
  if (v == 0) cpu.eflags |= ZF;
  if (v & 0x80000000u) cpu.eflags |= SF;
  {
    uint8_t lo = (uint8_t)v;
    lo ^= lo >> 4; lo ^= lo >> 2; lo ^= lo >> 1;
    if (!(lo & 1)) cpu.eflags |= PF;
  }
}

static inline void set_szp8(uint8_t v) {
  cpu.eflags &= ~(ZF | SF | PF);
  if (v == 0) cpu.eflags |= ZF;
  if (v & 0x80) cpu.eflags |= SF;
  {
    uint8_t lo = v;
    lo ^= lo >> 4; lo ^= lo >> 2; lo ^= lo >> 1;
    if (!(lo & 1)) cpu.eflags |= PF;
  }
}

static uint32_t *reg32(int i) {
  switch (i) {
    case 0: return &cpu.eax; case 1: return &cpu.ecx; case 2: return &cpu.edx;
    case 3: return &cpu.ebx; case 4: return &cpu.esp; case 5: return &cpu.ebp;
    case 6: return &cpu.esi; default: return &cpu.edi;
  }
}
static uint8_t *reg8(int i) { /* 0..3 = al..bl, 4..7 = ah..bh */
  static uint8_t dummy;
  uint8_t *r = (uint8_t *)reg32(i & 3);
  if (i < 4) return r;
  (void)dummy;
  return r + 1;
}

static inline uint16_t rd16(uint32_t a) {
  return in_guest(a, 2) ? *(uint16_t *)gp(a) : 0;
}
static inline void wr16(uint32_t a, uint16_t v) {
  if (in_guest(a, 2)) *(uint16_t *)gp(a) = v;
}

/* ------------------------------------------------------------- 解码工具 */
static inline uint8_t imm8(void) { return rd8(cpu.eip++); }
static inline uint32_t imm32(void) {
  uint32_t v = rd32(cpu.eip);
  cpu.eip += 4;
  return v;
}
static inline int8_t rel8(void) { return (int8_t)rd8(cpu.eip++); }

/* raw = 原始 ModRM 字节。x87 的寄存器形式（d9 ff = fcos 之类）用**整字节**当操作码，
 * 所以必须保留它；只存 rm 字段是不够的。 */
typedef struct { int mod, reg, rm; uint32_t addr; int is_reg; uint8_t raw; } ModRM;

/* 计算 ModRM，返回操作数地址（is_reg=1 时 addr 是寄存器编号） */
static ModRM modrm(void) {
  ModRM m;
  uint8_t b = imm8();
  m.raw = b;
  m.mod = b >> 6;
  m.reg = (b >> 3) & 7;
  m.rm = b & 7;
  m.is_reg = (m.mod == 3);
  m.addr = 0;
  if (m.is_reg) {
    m.addr = m.rm;
    return m;
  }
  uint32_t base = 0;
  switch (m.rm) {
    case 0: base = cpu.eax; break;
    case 1: base = cpu.ecx; break;
    case 2: base = cpu.edx; break;
    case 3: base = cpu.ebx; break;
    case 4: { /* SIB */
      uint8_t sib = imm8();
      int ss = sib >> 6, idx = (sib >> 3) & 7, bs = sib & 7;
      uint32_t idxv = (idx == 4) ? 0 : *reg32(idx);
      uint32_t bsv;
      /* SIB 里 base==5 且 mod==0 表示 base 是 disp32（与 scale 无关！）。
       * 之前多写了个 (sib>>6)==0 的条件，导致 scale 非 0 的跳转表
       * （jmpl *table(,%ecx,4)）被当成 base=ebp，直接跳到栈上去执行。 */
      if (bs == 5 && m.mod == 0) bsv = imm32();
      else bsv = (bs == 5) ? cpu.ebp : *reg32(bs);
      base = bsv + (idxv << ss);
      break;
    }
    case 5: base = (m.mod == 0) ? imm32() : cpu.ebp; break;
    case 6: base = cpu.esi; break;
    default: base = cpu.edi; break;
  }
  if (m.mod == 1) base += (int8_t)imm8();
  else if (m.mod == 2) base += imm32();
  if (g_seg_fs) base += TIB_BASE;  /* fs:[x] → TIB 影子区 */
  m.addr = base;
  return m;
}

/* ------------------------------------------------------------- 指令实现 */
static int g_steps = 0;
static int g_last_rc = -1;
/* 单次 CallDLL 的步数上限。**注意**：20M 曾把「合法的 10MB 级拷贝」误判成死循环，
 * 排查时提到 200M（≈25M 次 4 字节拷贝）。真要防死循环，靠下面的 eip 环判断更可靠。 */
static int g_max_steps = 200000000;
static int g_verbose = 0;

/* 最近执行过的指令地址（环形），失败时打出来用于回溯 */
#define EMU_HIST 32
static uint32_t g_hist[EMU_HIST];
static int g_hist_n = 0;
static void hist_add(uint32_t eip) { g_hist[g_hist_n++ % EMU_HIST] = eip; }
static void hist_dump(void) {
  int cnt = g_hist_n < EMU_HIST ? g_hist_n : EMU_HIST;
  fprintf(stderr, "[emu] 最近 %d 条指令地址：", cnt);
  for (int i = 0; i < cnt; ++i) {
    int k = (g_hist_n - cnt + i + EMU_HIST * 2) % EMU_HIST;
    fprintf(stderr, " %08x", g_hist[k]);
  }
  fprintf(stderr, "\n");
}

/* 按 4 字节打印来宾内存窗口（越界一律退化成 0，不会崩）。 */
static void dump_words(const char *tag, uint32_t a, int nwords) {
  fprintf(stderr, "[emu] %s @%08x:", tag, a);
  for (int i = 0; i < nwords; ++i)
    fprintf(stderr, " %08x", rd32(a + 4u * (uint32_t)i));
  fprintf(stderr, "\n");
}

/* 观察点环形缓冲：每次 CallDLL 首次命中只记一条，所以「卡住的那次调用」的入口
 * 现场一定在环里（步数上限时 200M 步的 add 早就把寄存器推歪了，事后看没用）。 */
#define WATCH_RING 16
static struct {
  uint32_t eip, eax, ebx, ecx, edx, esi, edi, ebp, esp;
  uint32_t win[12]; /* edi-0x10 .. edi+0x1c */
  uint32_t stk[6];  /* esp .. esp+0x14 */
  int valid;
} g_watch_ring[WATCH_RING];
static int g_watch_pos = 0;

static void watch_record(void) {
  int k = g_watch_pos++ % WATCH_RING;
  g_watch_ring[k].eip = cpu.eip;
  g_watch_ring[k].eax = cpu.eax;
  g_watch_ring[k].ebx = cpu.ebx;
  g_watch_ring[k].ecx = cpu.ecx;
  g_watch_ring[k].edx = cpu.edx;
  g_watch_ring[k].esi = cpu.esi;
  g_watch_ring[k].edi = cpu.edi;
  g_watch_ring[k].ebp = cpu.ebp;
  g_watch_ring[k].esp = cpu.esp;
  for (int i = 0; i < 12; ++i)
    g_watch_ring[k].win[i] = rd32(cpu.edi - 0x10u + 4u * (uint32_t)i);
  for (int i = 0; i < 6; ++i)
    g_watch_ring[k].stk[i] = rd32(cpu.esp + 4u * (uint32_t)i);
  g_watch_ring[k].valid = 1;
}

static void watch_dump_ring(void) {
  int cnt = g_watch_pos < WATCH_RING ? g_watch_pos : WATCH_RING;
  fprintf(stderr, "[emu] WATCH 环（%d 条，旧->新），最后一条就是卡住的那次调用：\n",
          cnt);
  for (int i = 0; i < cnt; ++i) {
    int k = (g_watch_pos - cnt + i + WATCH_RING * 2) % WATCH_RING;
    if (!g_watch_ring[k].valid) continue;
    fprintf(stderr,
            "[emu]  W%d eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x "
            "edi=%08x ebp=%08x esp=%08x\n",
            i, g_watch_ring[k].eip, g_watch_ring[k].eax, g_watch_ring[k].ebx,
            g_watch_ring[k].ecx, g_watch_ring[k].edx, g_watch_ring[k].esi,
            g_watch_ring[k].edi, g_watch_ring[k].ebp, g_watch_ring[k].esp);
    fprintf(stderr, "[emu]   edi-0x10:");
    for (int j = 0; j < 12; ++j) fprintf(stderr, " %08x", g_watch_ring[k].win[j]);
    fprintf(stderr, "\n[emu]   esp    :");
    for (int j = 0; j < 6; ++j) fprintf(stderr, " %08x", g_watch_ring[k].stk[j]);
    fprintf(stderr, "\n");
  }
}

/* intD 读取的环形缓冲：死循环是「最后一次调用」卡住的，所以步数上限时环里剩的
 * 恰好就是那次调用读过的槽位 —— 拿去和 PC 侧 --full 的 intD 逐项对比即可。 */
#define INTDRD_RING 512
static struct { uint32_t idx, val, eip; } g_intd_ring[INTDRD_RING];
static int g_intd_ring_pos = 0;

static void intd_ring_add(uint32_t idx, uint32_t val, uint32_t eip) {
  int k = g_intd_ring_pos++ % INTDRD_RING;
  g_intd_ring[k].idx = idx;
  g_intd_ring[k].val = val;
  g_intd_ring[k].eip = eip;
}

static void intd_dump_ring(void) {
  int cnt = g_intd_ring_pos < INTDRD_RING ? g_intd_ring_pos : INTDRD_RING;
  fprintf(stderr, "[emu] INTD 读环（%d 条，旧->新；卡住那次调用的读取）:\n", cnt);
  for (int i = 0; i < cnt; ++i) {
    int k = (g_intd_ring_pos - cnt + i + INTDRD_RING * 2) % INTDRD_RING;
    fprintf(stderr, "[emu]  R intD[%u]=%d @%08x\n", g_intd_ring[k].idx,
            (int)g_intd_ring[k].val, g_intd_ring[k].eip);
  }
}

#define UNIMPL(op)                                                       \
  do {                                                                   \
    fprintf(stderr,                                                      \
            "[emu] 未实现指令 %s @ %08x（已读到 eip=%08x，字节 %02x %02x %02x %02x）\n", \
            op, g_insn_addr, cpu.eip, rd8(g_insn_addr), rd8(g_insn_addr + 1), \
            rd8(g_insn_addr + 2), rd8(g_insn_addr + 3));                  \
    hist_dump();                                                         \
    return -1;                                                           \
  } while (0)

static void push32(uint32_t v) { cpu.esp -= 4; wr32(cpu.esp, v); }
static uint32_t pop32(void) { uint32_t v = rd32(cpu.esp); cpu.esp += 4; return v; }

/* 调用栈 + 栈平衡断言：每次 call 记下「应当被 ret 弹出的返回地址」，
 * 每次 ret 校验实际栈顶是否等于它。不等就说明中间某条指令把 esp 顶歪了
 * （之前遇到的 stdcall 清栈漏做、操作数宽度写错都属于这一类），
 * 这里当场报出差了多少字节 + 调用链，省得只能看到“最后跳到 0”。 */
#define EMU_CALLDEPTH 64
typedef struct { uint32_t ret_addr, callee, esp_entry; } EmuFrame;
static EmuFrame g_frames[EMU_CALLDEPTH];
static int g_frame_n = 0;

static void emu_call_push(uint32_t callee, uint32_t ret_addr, uint32_t esp_entry) {
  if (g_frame_n < EMU_CALLDEPTH) {
    g_frames[g_frame_n].ret_addr = ret_addr;
    g_frames[g_frame_n].callee = callee;
    g_frames[g_frame_n].esp_entry = esp_entry;
    ++g_frame_n;
    return;
  }
  /* 调用深度溢出：以前仍然 ++g_frame_n，于是 g_frame_n 一路涨到几百万，
   * 后面「打印调用栈」的循环 g_frames[g_frame_n-1] 就越界 → 报错路径自己 SIGSEGV。
   * 现在溢出后**不再增长**，只记一次标志；栈不平衡时照样会被 emu_ret_check 抓到。 */
  static int warned = 0;
  if (!warned) {
    warned = 1;
    fprintf(stderr, "[emu] 调用深度超过 %d（后续帧不再记录）\n", EMU_CALLDEPTH);
  }
}

static int emu_ret_check(void) {
  if (g_frame_n <= 0 || g_frame_n > EMU_CALLDEPTH) {
    fprintf(stderr, "[emu] ret 时调用栈为空（栈或调用记录已失配）\n");
    return -1;
  }
  EmuFrame f = g_frames[--g_frame_n];
  uint32_t actual = rd32(cpu.esp);
  if (actual != f.ret_addr) {
    fprintf(stderr,
            "[emu] 栈不平衡：ret 期望 %08x，栈顶实际 %08x\n"
            "       callee=%08x esp_entry=%08x 现在 esp=%08x 差 %d 字节\n"
            "       调用链：",
            f.ret_addr, actual, f.callee, f.esp_entry, cpu.esp,
            (int)(cpu.esp - f.esp_entry));
    for (int i = g_frame_n - 1; i >= 0 && i > g_frame_n - 8; --i)
      fprintf(stderr, " <- %08x", g_frames[i].callee);
    fprintf(stderr, "\n");
    return -1;
  }
  cpu.esp += 4;
  return 0;
}

static uint32_t rm_read32(ModRM m) {
  return m.is_reg ? *reg32(m.addr) : rd32(m.addr);
}
static void rm_write32(ModRM m, uint32_t v) {
  if (m.is_reg) *reg32(m.addr) = v; else wr32(m.addr, v);
}
static uint8_t rm_read8(ModRM m) {
  return m.is_reg ? *reg8(m.addr) : rd8(m.addr);
}
static void rm_write8(ModRM m, uint8_t v) {
  if (m.is_reg) *reg8(m.addr) = v; else wr8(m.addr, v);
}

static void alu_add(uint32_t a, uint32_t b) {
  uint64_t r = (uint64_t)a + b;
  uint32_t v = (uint32_t)r;
  cpu.eflags &= ~(CF | OF);
  if (r > 0xffffffffu) cpu.eflags |= CF;
  if (((a ^ v) & (b ^ v)) & 0x80000000u) cpu.eflags |= OF;
  set_szp32(v);
}
static void alu_sub(uint32_t a, uint32_t b) {
  uint32_t v = a - b;
  cpu.eflags &= ~(CF | OF);
  if (a < b) cpu.eflags |= CF;
  if (((a ^ b) & (a ^ v)) & 0x80000000u) cpu.eflags |= OF;
  set_szp32(v);
}
static void alu_cmp(uint32_t a, uint32_t b) { alu_sub(a, b); }
/* sbb：带借位减（旧 CF 先参与，再用 33 位比较决定新的借位） */
static uint32_t alu_sbb(uint32_t a, uint32_t b) {
  uint64_t sub = (uint64_t)b + ((cpu.eflags & CF) ? 1u : 0u);
  uint32_t v = a - (uint32_t)sub;
  cpu.eflags &= ~(CF | OF);
  if ((uint64_t)a < sub) cpu.eflags |= CF;
  if (((a ^ (uint32_t)sub) & (a ^ v)) & 0x80000000u) cpu.eflags |= OF;
  set_szp32(v);
  return v;
}

/* 条件跳转：cc = 0..15（o no b ae e ne be a s ns p np l ge le g） */
static int cond(int cc) {
  int cf = !!(cpu.eflags & CF), zf = !!(cpu.eflags & ZF);
  int sf = !!(cpu.eflags & SF), of = !!(cpu.eflags & OF), pf = !!(cpu.eflags & PF);
  switch (cc) {
    case 0x0: return of;           case 0x1: return !of;
    case 0x2: return cf;           case 0x3: return !cf;
    case 0x4: return zf;           case 0x5: return !zf;
    case 0x6: return cf || zf;     case 0x7: return !cf && !zf;
    case 0x8: return sf;           case 0x9: return !sf;
    case 0xA: return pf;           case 0xB: return !pf;
    case 0xC: return sf != of;     case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    default: return !zf && (sf == of);
  }
}

static int step(void);

/* ------------------------------------------------------------------ x87 */
/* 只需要覆盖 PT00 实际用到的那一小族：fld/fst/fstp（m32/m64）、fild/fistp、
 * fadd/fsub/fsubr/fmul/fdiv/fdivr、fcom/fcomp、faddp/fmulp/fsubp/fsubrp/
 * fdivp/fdivrp、fld1/fldz/fchs/fabs/fsqrt/fsin/fcos/fpatan、fldcw/fnstcw/fnstsw。
 * 栈用 8 个 double 模拟（x87 内部是 80 位，先按 double 走，精度差异后面再对齐）。 */
static double g_fp[8];
static int g_fptop = 0;
static double *XP(int i) { return &g_fp[(g_fptop + i) & 7]; }
static void fp_push(double v) { g_fptop = (g_fptop - 1) & 7; g_fp[g_fptop] = v; }
static double fp_pop(void) { double v = g_fp[g_fptop]; g_fptop = (g_fptop + 1) & 7; return v; }

/* x87 状态字：比较结果放在 C3(bit14)/C2(bit10)/C0(bit8)。
 * 这是必须实现的——编译器的浮点比较惯用法是
 *     fcompl / fnstsw ax / test ah,0x41 / sahf / jcc
 * 不写状态字的话 fnstsw 恒为 0，后面那个分支永远走同一边，
 * 函数就会走错路径（func 10 的 3D 投影就是这么偏掉的）。 */
static uint16_t g_fpu_sw = 0;
static uint16_t g_fpu_cw = 0x027f; /* 控制字：bit8-9 精度、bit10-11 舍入 */

/* 按控制字的舍入模式把浮点转成整数（`_ftol2` 会先用 fldcw 切到"向零取整"）。 */
static int64_t fp_to_int(double v) {
  switch ((g_fpu_cw >> 10) & 3) {
    case 0: return (int64_t)(v < 0 ? v - 0.5 : v + 0.5); /* 就近 */
    case 1: return (int64_t)floor(v);
    case 2: return (int64_t)ceil(v);
    default: return (int64_t)v;                          /* 向零 */
  }
}
static void fp_cmp(double a, double b) {
  g_fpu_sw &= (uint16_t)~(0x0100u | 0x0400u | 0x4000u); /* 清 C0/C2/C3 */
  if (a > b) {
    /* C3=0 C2=0 C0=0 */
  } else if (a < b) {
    g_fpu_sw |= 0x0100u; /* C0 */
  } else {
    g_fpu_sw |= 0x4000u; /* C3：相等 */
  }
}

/* 返回 0 = 已处理；-1 = 未实现 */
/* log2：fyl2x/fyl2xp1 用（MSVC 没有 lg2） */
static double fp_log2(double v) { return log(v) / log(2.0); }

static int x87(uint8_t op) {
  ModRM m = modrm();
  const int reg = m.reg;
  if (op == 0xd8) { /* m32real 与 st 形式 */
    if (m.mod != 3) {
      float v = in_guest(m.addr, 4) ? *(float *)gp(m.addr) : 0.0f;
      switch (reg) {
        case 0: *XP(0) += v; return 0;
        case 1: *XP(0) *= v; return 0;
        case 2: fp_cmp(*XP(0), v); return 0;                        /* fcom m32 */
        case 3: fp_cmp(*XP(0), v); fp_pop(); return 0;                /* fcomp m32 */
        case 4: *XP(0) -= v; return 0;
        case 5: *XP(0) = v - *XP(0); return 0;
        case 6: *XP(0) /= v; return 0;
        case 7: *XP(0) = v / *XP(0); return 0;
      }
    } else {
      switch (reg) {
        case 0: *XP(0) += *XP(m.rm); return 0;
        case 1: *XP(0) *= *XP(m.rm); return 0;
        case 2: fp_cmp(*XP(0), *XP(m.rm)); return 0;                  /* fcom st(i) */
        case 3: fp_cmp(*XP(0), *XP(m.rm)); fp_pop(); return 0;        /* fcomp st(i) */
        case 4: *XP(0) -= *XP(m.rm); return 0;
        case 5: { double t = *XP(m.rm) - *XP(0); *XP(0) = t; return 0; }
        case 6: *XP(0) /= *XP(m.rm); return 0;
        case 7: { double t = *XP(m.rm) / *XP(0); *XP(0) = t; return 0; }
      }
    }
  } else if (op == 0xd9) {
    /* 一律走 gpc()：越界只记一笔，不再把整台引擎带走 */
    if (m.mod != 3 && reg == 0) {
      uint8_t *p = gpc(m.addr, 4, "fld m32");
      fp_push(p ? *(float *)p : 0.0f);
      return 0;
    }
    if (m.mod != 3 && reg == 2) {
      uint8_t *p = gpc(m.addr, 4, "fst m32");
      if (p) *(float *)p = (float)*XP(0);
      return 0;
    }
    if (m.mod != 3 && reg == 3) {
      uint8_t *p = gpc(m.addr, 4, "fstp m32");
      if (p) *(float *)p = (float)fp_pop();
      return 0;
    }
    if (m.mod != 3 && reg == 5) { g_fpu_cw = rd16(m.addr); return 0; } /* fldcw */
    if (m.mod != 3 && reg == 7) { wr16(m.addr, g_fpu_cw); return 0; }  /* fnstcw */
    if (m.mod == 3) {
      if (m.raw >= 0xc0 && m.raw <= 0xc7) { fp_push(*XP(m.raw - 0xc0)); return 0; }
      if (m.raw >= 0xc8 && m.raw <= 0xcf) {                     /* fxch */
        double t = *XP(0); *XP(0) = *XP(m.raw - 0xc8); *XP(m.raw - 0xc8) = t; return 0;
      }
      switch (m.raw) {
        case 0xe0: *XP(0) = -*XP(0); return 0;   /* fchs */
        case 0xe1: *XP(0) = *XP(0) < 0 ? -*XP(0) : *XP(0); return 0; /* fabs */
        case 0xe4: fp_cmp(*XP(0), 0.0); return 0;                       /* ftst */
        case 0xe8: fp_push(1.0); return 0;       /* fld1 */
        case 0xe9: fp_push(3.3219280948873623); return 0;               /* fldl2t   = log2(10) */
        case 0xea: fp_push(1.4426950408889634); return 0;               /* fldl2e   = log2(e)  */
        case 0xeb: fp_push(3.1415926535897932); return 0;               /* fldpi */
        case 0xec: fp_push(0.3010299956639812); return 0;               /* fldlg2   = log10(2) */
        case 0xed: fp_push(0.6931471805599453); return 0;               /* fldln2   = ln(2)   */
        case 0xee: fp_push(0.0); return 0;       /* fldz */
        case 0xf0: *XP(0) = pow(2.0, *XP(0)) - 1.0; return 0;           /* f2xm1 */
        case 0xf1: { double t = *XP(1) * fp_log2(*XP(0)); fp_pop(); *XP(0) = t; return 0; } /* fyl2x */
        case 0xf2: { double t = tan(*XP(0)); *XP(0) = t; fp_push(1.0); return 0; }   /* fptan */
        case 0xf3: { double t = atan2(*XP(1), *XP(0)); fp_pop(); *XP(0) = t; return 0; } /* fpatan */
        case 0xf5: case 0xf8: *XP(0) = fmod(*XP(0), *XP(1)); return 0;  /* fprem1/fprem（近似） */
        case 0xf6: case 0xf7: return 0;                                 /* fdecstp/fincstp：本模型不需要 */
        case 0xf9: { double t = *XP(1) * fp_log2(*XP(0) + 1.0); fp_pop(); *XP(0) = t; return 0; } /* fyl2xp1 */
        case 0xfa: *XP(0) = sqrt(*XP(0)); return 0;
        case 0xfb: { double s0 = sin(*XP(0)), c0 = cos(*XP(0)); *XP(0) = s0; fp_push(c0); return 0; } /* fsincos */
        case 0xfc: *XP(0) = ((g_fpu_cw >> 10) & 3) == 3 ? trunc(*XP(0)) : nearbyint(*XP(0)); return 0;
        case 0xfe: *XP(0) = sin(*XP(0)); return 0;
        case 0xff: *XP(0) = cos(*XP(0)); return 0;
      }
    }
  } else if (op == 0xdb && m.mod != 3) { /* fild m32 / fistp m32 */
    if (reg == 0) {
      uint8_t *p = gpc(m.addr, 4, "fild m32");
      fp_push(p ? (double)*(int32_t *)p : 0.0);
      return 0;
    }
    if (reg == 3) { uint32_t v = (uint32_t)fp_to_int(*XP(0)); fp_pop(); wr32(m.addr, v); return 0; }
    if (reg == 2) { wr32(m.addr, (uint32_t)fp_to_int(*XP(0))); return 0; } /* fist m32 */
  } else if (op == 0xda && m.mod != 3) { /* m32int 整数算术（FIADD…FIDIVR）
     * 真机正是死在这一族的缺失实现上：0x100033ED 的 `da 46 4c`（fiadd dword
     * [esi+0x4c]）以前报「未实现指令」→ 每次 CallDLL 都被中途放弃，小游戏被
     * 截断成"半执行"（能画但逻辑不推进）。 */
    uint8_t *p = gpc(m.addr, 4, "m32int");
    double v = p ? (double)*(int32_t *)p : 0.0;
    switch (reg) {
      case 0: *XP(0) += v; return 0;
      case 1: *XP(0) *= v; return 0;
      case 2: fp_cmp(*XP(0), v); return 0;
      case 3: fp_cmp(*XP(0), v); fp_pop(); return 0;
      case 4: *XP(0) -= v; return 0;
      case 5: *XP(0) = v - *XP(0); return 0;
      case 6: *XP(0) /= v; return 0;
      case 7: *XP(0) = v / *XP(0); return 0;
    }
  } else if (op == 0xda && m.mod == 3 && m.raw == 0xe9) { /* fucompp */
    fp_cmp(*XP(0), *XP(1));
    fp_pop();
    fp_pop();
    return 0;
  } else if (op == 0xdb && m.mod == 3 && (m.raw == 0xe2 || m.raw == 0xe3)) { /* fnclex/fninit */
    g_fpu_sw = 0;
    return 0;
  } else if (op == 0xdc) { /* m64real */
    if (m.mod != 3) {
      uint8_t *pv = gpc(m.addr, 8, "m64real");
      double v = pv ? *(double *)pv : 0.0;
      switch (reg) {
        case 0: *XP(0) += v; return 0;
        case 1: *XP(0) *= v; return 0;
        case 2: fp_cmp(*XP(0), v); return 0;          /* fcom m64 */
        case 3: fp_cmp(*XP(0), v); fp_pop(); return 0; /* fcompl m64 */
        case 4: *XP(0) -= v; return 0;
        case 5: *XP(0) = v - *XP(0); return 0;
        case 6: *XP(0) /= v; return 0;
        case 7: *XP(0) = v / *XP(0); return 0;
      }
    } else {
      switch (reg) {
        case 0: *XP(m.rm) += *XP(0); return 0;
        case 1: *XP(m.rm) *= *XP(0); return 0;
        case 2: fp_cmp(*XP(0), *XP(m.rm)); return 0;          /* fcom st(i) */
        case 3: fp_cmp(*XP(0), *XP(m.rm)); fp_pop(); return 0; /* fcomp st(i) */
        /* DC 组：E0+i = FSUBR st(i),st0（st(i)=st0-st(i)）；E8+i = FSUB；F0+i = FDIVR；F8+i = FDIV。
         * 注意 x87 的怪癖：**D8 组的 E0/E8 与 DC/DE 组正好相反**，D8 那边是对的，别一起改。 */
        case 4: { double t = *XP(0) - *XP(m.rm); *XP(m.rm) = t; return 0; }
        case 5: *XP(m.rm) -= *XP(0); return 0;
        case 6: { double t = *XP(0) / *XP(m.rm); *XP(m.rm) = t; return 0; }
        case 7: *XP(m.rm) /= *XP(0); return 0;
      }
    }
  } else if (op == 0xdd && m.mod != 3) { /* fld/fst/fstp m64 */
    if (reg == 0) {
      uint8_t *p = gpc(m.addr, 8, "fld m64");
      fp_push(p ? *(double *)p : 0.0);
      return 0;
    }
    if (reg == 2) {
      uint8_t *p = gpc(m.addr, 8, "fst m64");
      if (p) *(double *)p = *XP(0);
      return 0;
    }
    if (reg == 3) {
      uint8_t *p = gpc(m.addr, 8, "fstp m64");
      if (p) *(double *)p = fp_pop();
      return 0;
    }
  } else if (op == 0xdd) { /* dd 寄存器形式：ffree / fst st(i) / fstp st(i) / fucom(p) */
    if (m.raw >= 0xc0 && m.raw <= 0xc7) return 0;                                   /* ffree st(i) */
    if (m.raw >= 0xd0 && m.raw <= 0xd7) { *XP(m.raw - 0xd0) = *XP(0); return 0; }   /* fst st(i) */
    if (m.raw >= 0xd8 && m.raw <= 0xdf) { *XP(m.raw - 0xd8) = fp_pop(); return 0; } /* fstp st(i) */
    if (m.raw >= 0xe0 && m.raw <= 0xe7) { fp_cmp(*XP(0), *XP(m.raw - 0xe0)); return 0; }
    if (m.raw >= 0xe8 && m.raw <= 0xef) { fp_cmp(*XP(0), *XP(m.raw - 0xe8)); fp_pop(); return 0; }
  } else if (op == 0xde) { /* 出栈式算术 + fcompp；内存形式是 m16int 整数算术 */
    if (m.mod != 3) {
      /* DE /0../7 = FIADD/FIMUL/FICOM/FICOMP/FISUB/FISUBR/FIDIV/FIDIVR m16int。
       * 以前 reg==0 只做 fp_pop()（把 st0 弹掉），是错的 —— 整数加法会顺手毁掉
       * 整个 FPU 栈。 */
      uint8_t *p = gpc(m.addr, 2, "m16int");
      double v = p ? (double)*(int16_t *)p : 0.0;
      switch (reg) {
        case 0: *XP(0) += v; return 0;
        case 1: *XP(0) *= v; return 0;
        case 2: fp_cmp(*XP(0), v); return 0;
        case 3: fp_cmp(*XP(0), v); fp_pop(); return 0;
        case 4: *XP(0) -= v; return 0;
        case 5: *XP(0) = v - *XP(0); return 0;
        case 6: *XP(0) /= v; return 0;
        case 7: *XP(0) = v / *XP(0); return 0;
      }
      return 0;
    }
    if (m.mod == 3) {
      if (m.raw == 0xd9) { /* fcompp */
        fp_cmp(*XP(0), *XP(1));
        fp_pop();
        fp_pop();
        return 0;
      }
      switch (reg) {
        case 0: *XP(1) += *XP(0); fp_pop(); return 0;  /* faddp */
        case 1: *XP(1) *= *XP(0); fp_pop(); return 0;  /* fmulp */
        /* DE 组：E0+i = FSUBRP st(i),st0（st(i)=st0-st(i) 后 pop）；E8+i = FSUBP（st(i)=st(i)-st0）。
         * 球 B 的抛物线 `v0*t - 0.6t²` 用的就是 DE E9（FSUBP），原先方向反了 → 落点符号错 → 复位判定走另一支。 */
        case 4: { double t = *XP(0) - *XP(1); fp_pop(); *XP(0) = t; return 0; }
        case 5: { double t = *XP(1) - *XP(0); fp_pop(); *XP(0) = t; return 0; }
        case 6: { double t = *XP(0) / *XP(1); fp_pop(); *XP(0) = t; return 0; }
        case 7: { double t = *XP(1) / *XP(0); fp_pop(); *XP(0) = t; return 0; }
      }
    }
  } else if (op == 0xdf) {
    if (m.mod == 3 && m.raw == 0xe0) { /* fnstsw ax */
      cpu.eax = (cpu.eax & 0xffff0000u) | (uint32_t)g_fpu_sw;
      return 0;
    }
    if (m.mod != 3 && reg == 5) { /* fild m16 */
      uint8_t *p = gpc(m.addr, 2, "fild m16");
      fp_push(p ? (double)*(int16_t *)p : 0.0);
      return 0;
    }
    /* DF /7 = fistpll：**8 字节** int64（不是 int16）。写错的话，紧接着的
     * `movl -0x8(%ebp),%edx` 会读到旧垃圾，整数结果就变成 0xEFDFxxxx 那种值。 */
    if (m.mod != 3 && reg == 7) {
      uint64_t v = (uint64_t)fp_to_int(*XP(0));
      fp_pop();
      wr32(m.addr, (uint32_t)v);
      wr32(m.addr + 4, (uint32_t)(v >> 32));
      return 0;
    }
  }
  return -1;
}

/* ---------------------------------------------------------- 导入桩分发 */
static uint32_t arg_at(int i) { return rd32(cpu.esp + 4 + 4 * i); }
static int stub_is(int idx, const char *name) {
  return strcmp(g_stub_name[idx], name) == 0;
}

/* KERNEL32 是 stdcall：被调方负责清参数。桩必须照做，否则栈会每个调用漂 4 字节，
 * 攒到最后 ret 弹出来的就不是返回地址了（真机上表现为跳到假堆地址）。 */
/* TLS 槽：DLL 的 CRT 用 __getptd()（TlsGetValue/TlsSetValue + GetCurrentThreadId）
 * 保存 per-thread 数据，rand() 的状态就存在 ptd+0x14。之前 TlsGetValue 一律返回 0
 * → 每次 rand() 都新分配一个 ptd → 随机序列与原生分叉（表现为只有用 rand 的实体类型对不上）。 */
#define EMU_TLS_SLOTS 64
static uint32_t g_tls_val[EMU_TLS_SLOTS];
static int g_tls_used[EMU_TLS_SLOTS];

/* 槽位映射：TlsAlloc 分配出来的索引直接用；**CRT 自己没走 TlsAlloc 时**索引会是 -1
 * （我们不跑 DllMain，见 main 里的说明），这时把它退化成 0 号槽——重点不是索引对不对，
 * 而是"同一个索引始终映射到同一个槽"，这样 ptd（含 rand 状态）才持久。 */
static int tls_slot(uint32_t idx) {
  if (idx < EMU_TLS_SLOTS) return (int)idx;
  return 0;
}

static int stub_argc(int idx) {
  static const struct { const char *n; int c; } kTable[] = {
      {"GetSystemTime", 1}, {"GetLocalTime", 1}, {"SetLastError", 1},
      {"SetUnhandledExceptionFilter", 1}, {"GetTimeZoneInformation", 1},
      {"RtlUnwind", 4}, {"GetEnvironmentVariableA", 3}, {"GetProcAddress", 2},
      {"LoadLibraryA", 1}, {"DeleteCriticalSection", 1},
      {"EnterCriticalSection", 1}, {"LeaveCriticalSection", 1},
      {"InitializeCriticalSection", 1}, {"GetModuleHandleA", 1},
      {"HeapCreate", 3}, {"HeapAlloc", 3}, {"HeapReAlloc", 4},
      {"VirtualAlloc", 4}, {"HeapFree", 3}, {"VirtualFree", 3},
      {"HeapDestroy", 1}, {"IsBadReadPtr", 2}, {"IsBadWritePtr", 2},
      {"HeapSize", 3}, {"TlsGetValue", 1}, {"TlsSetValue", 2}, {"TlsFree", 1},
      {"InterlockedIncrement", 1}, {"InterlockedDecrement", 1}, {"time", 1},
      {"GetStdHandle", 1}, {"GetFileType", 1}, {"WriteFile", 5},
      {"ExitProcess", 1}, {"TerminateProcess", 2},
  };
  for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); ++i) {
    if (strcmp(g_stub_name[idx], kTable[i].n) == 0) return kTable[i].c;
  }
  return 0;
}

/* 返回 0 表示"已处理、按 cdecl 返回"，返回 1 表示是 ExitProcess 之类要停机 */
static int stub_call(int idx) {
  ++g_stub_hits[idx];
  if (g_stub_hits[idx] == 1)
    fprintf(stderr, "[emu] 桩命中 %s\n", g_stub_name[idx]);

  if (stub_is(idx, "GetSystemTime") || stub_is(idx, "GetLocalTime")) {
    uint32_t p = arg_at(0); /* SYSTEMTIME*，全 0 即可 */
    for (int i = 0; i < 16; i += 4) wr32(p + i, 0);
    cpu.eax = 0;
  } else if (stub_is(idx, "GetVersion") || stub_is(idx, "GetLastError") ||
             stub_is(idx, "SetLastError") || stub_is(idx, "SetUnhandledExceptionFilter") ||
             stub_is(idx, "GetTimeZoneInformation") || stub_is(idx, "RtlUnwind") ||
             stub_is(idx, "GetEnvironmentVariableA") || stub_is(idx, "GetProcAddress") ||
             stub_is(idx, "LoadLibraryA") || stub_is(idx, "DeleteCriticalSection") ||
             stub_is(idx, "EnterCriticalSection") || stub_is(idx, "LeaveCriticalSection") ||
             stub_is(idx, "InitializeCriticalSection")) {
    cpu.eax = 0;
  } else if (stub_is(idx, "GetModuleHandleA")) {
    cpu.eax = IMAGE_BASE;
  } else if (stub_is(idx, "GetModuleFileNameA")) {
    /* 返回一个像样的模块名并 NUL 结尾：CRT 会拿它做初始化，返回空串更容易踩到怪路径 */
    static const char kMod[] = "PT00.dll";
    uint32_t buf = arg_at(1), n = arg_at(2), i = 0;
    if (buf && n) {
      for (; kMod[i] && i + 1 < n; ++i) wr8(buf + i, (uint8_t)kMod[i]);
      wr8(buf + i, 0);
      cpu.eax = i;
    } else {
      cpu.eax = 0;
    }
  } else if (stub_is(idx, "GetCurrentProcess") || stub_is(idx, "GetCurrentThreadId")) {
    cpu.eax = 1;
  } else if (stub_is(idx, "GetCommandLineA")) {
    static uint32_t s = 0;
    if (!s) { s = guest_alloc(4); g_mem[s - GUEST_BASE] = 0; }
    cpu.eax = s;
  } else if (stub_is(idx, "HeapCreate") || stub_is(idx, "GetProcessHeap")) {
    cpu.eax = 0x00d00000u;
  } else if (stub_is(idx, "HeapAlloc")) {
    /* HeapAlloc(hHeap, dwFlags, dwBytes) */
    uint32_t sz = arg_at(2) ? arg_at(2) : 64;
    uint32_t p = guest_alloc(sz);
    heap_note(p, sz);
    cpu.eax = p;
  } else if (stub_is(idx, "HeapReAlloc")) {
    /* HeapReAlloc(hHeap, dwFlags, lpMem, dwBytes)：**必须真的搬到新块并拷贝**。
     * 老实现直接返回 lpMem —— DLL 以为缓冲区变大了，写进去就踩坏隔壁数据。 */
    uint32_t oldp = arg_at(2), sz = arg_at(3) ? arg_at(3) : 64;
    uint32_t oldsz = heap_size_of(oldp);
    uint32_t np = guest_alloc(sz);
    if (oldp && oldsz) {
      uint32_t n = oldsz < sz ? oldsz : sz;
      for (uint32_t i = 0; i < n; ++i) wr8(np + i, rd8(oldp + i));
    }
    heap_note(np, sz);
    cpu.eax = np;
  } else if (stub_is(idx, "VirtualAlloc")) {
    /* VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) */
    uint32_t sz = arg_at(1) ? arg_at(1) : 4096;
    uint32_t p = guest_alloc(sz);
    heap_note(p, sz);
    cpu.eax = p;
  } else if (stub_is(idx, "HeapFree") || stub_is(idx, "VirtualFree") ||
             stub_is(idx, "HeapDestroy") || stub_is(idx, "IsBadReadPtr") ||
             stub_is(idx, "IsBadWritePtr")) {
    cpu.eax = stub_is(idx, "HeapFree") || stub_is(idx, "VirtualFree") ? 1 : 0;
  } else if (stub_is(idx, "HeapSize")) {
    cpu.eax = heap_size_of(arg_at(2));
  } else if (stub_is(idx, "TlsAlloc")) {
    int s = -1;
    for (int i = 0; i < EMU_TLS_SLOTS; ++i) if (!g_tls_used[i]) { s = i; break; }
    if (s < 0) { cpu.eax = 0xffffffffu; }
    else { g_tls_used[s] = 1; g_tls_val[s] = 0; cpu.eax = (uint32_t)s; }
  } else if (stub_is(idx, "TlsGetValue")) {
    cpu.eax = g_tls_val[tls_slot(arg_at(0))];
  } else if (stub_is(idx, "TlsSetValue")) {
    g_tls_val[tls_slot(arg_at(0))] = arg_at(1);
    cpu.eax = 1;
  } else if (stub_is(idx, "TlsFree")) {
    int s = tls_slot(arg_at(0));
    g_tls_used[s] = 0;
    g_tls_val[s] = 0;
    cpu.eax = 1;
  } else if (stub_is(idx, "InterlockedIncrement")) {
    uint32_t p = arg_at(0);
    uint32_t v = rd32(p) + 1;
    wr32(p, v);
    cpu.eax = v;
  } else if (stub_is(idx, "InterlockedDecrement")) {
    uint32_t p = arg_at(0);
    uint32_t v = rd32(p) - 1;
    wr32(p, v);
    cpu.eax = v;
  } else if (stub_is(idx, "GetTickCount") || stub_is(idx, "time")) {
    cpu.eax = (uint32_t)time(NULL);
  } else if (stub_is(idx, "GetStdHandle")) {
    cpu.eax = 0xfffffff4u; /* 假的 STD_OUTPUT_HANDLE 值 */
  } else if (stub_is(idx, "GetFileType")) {
    cpu.eax = 2; /* FILE_TYPE_CHAR，让 CRT 认为有控制台 */
  } else if (stub_is(idx, "WriteFile")) {
    uint32_t written = arg_at(3);
    if (written) wr32(written, 0);
    cpu.eax = 1;
  } else if (stub_is(idx, "ExitProcess") || stub_is(idx, "TerminateProcess")) {
    fprintf(stderr, "[emu] 调用了 %s，停机；调用栈（callee<-caller_ret）：", g_stub_name[idx]);
    for (int i = g_frame_n - 1; i >= 0 && i > g_frame_n - 12; --i)
      fprintf(stderr, " %08x<-%08x", g_frames[i].callee, g_frames[i].ret_addr);
    fprintf(stderr, "\n");
    return 1;
  } else {
    static int warned = 0;
    if (warned++ < 20)
      fprintf(stderr, "[emu] 桩未实现细节：%s -> 返回 0\n", g_stub_name[idx]);
    cpu.eax = 0;
  }
  return 0;
}

static int step(void) {
  g_eip_ring[g_eip_ring_pos] = cpu.eip;
  g_eip_ring_pos = (g_eip_ring_pos + 1) % EIP_RING;
  if (++g_steps > g_max_steps) {
    g_last_hit_step_limit = 1;
    fprintf(stderr, "[emu] 步数上限 %d 用尽\n", g_max_steps);
    /* 关键现场：DLL 卡在死循环时的寄存器（指针/长度都在这里） */
    fprintf(stderr,
            "[emu] 寄存器 eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x "
            "ebp=%08x esp=%08x\n",
            cpu.eax, cpu.ebx, cpu.ecx, cpu.edx, cpu.esi, cpu.edi, cpu.ebp,
            cpu.esp);
    hist_dump();
    dump_words("mem edi-0x30", cpu.edi - 0x30u, 36);
    dump_words("mem esp", cpu.esp, 12);
    dump_words("mem esi", cpu.esi, 8);
    watch_dump_ring();
    intd_dump_ring();
    fprintf(stderr, "[emu] 调用栈：");
    for (int i = g_frame_n - 1; i >= 0 && i > g_frame_n - 10; --i)
      fprintf(stderr, " %08x<-%08x", g_frames[i].callee, g_frames[i].ret_addr);
    fprintf(stderr, "\n");
    return -1;
  }
  /* 落在导入桩区间：改调宿主实现，然后按 cdecl 返回 */
  if (cpu.eip >= STUB_BASE && cpu.eip < STUB_BASE + STUB_MAX * STUB_STRIDE) {
    int idx = (int)((cpu.eip - STUB_BASE) / STUB_STRIDE);
    if (stub_call(idx) != 0) return -1;
    uint32_t ret = rd32(cpu.esp);
    if (emu_ret_check() != 0) return -1;
    cpu.eip = ret;
    cpu.esp += (uint32_t)stub_argc(idx) * 4u;  /* stdcall：被调方清参数 */
    return 0;
  }
  if (g_trace && cpu.eip >= g_trace_min && g_trace_left-- > 0) {
    fprintf(stderr,
            "  %08x eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x"
            " st0=%.6g st1=%.6g st2=%.6g sw=%04x\n",
            cpu.eip, cpu.eax, cpu.ecx, cpu.edx, cpu.esp, cpu.ebp, *XP(0), *XP(1),
            *XP(2), g_fpu_sw);
    /* 前缀和表：参数错会让它把表后面一大片内存写花，单独打出来 */
    if (cpu.eip == IMAGE_BASE + 0x22D0) {
      fprintf(stderr, "      ^ 调 prefix_sum(dst=%08x src=%08x count=%d) 来自 %08x\n",
              rd32(cpu.esp + 4), rd32(cpu.esp + 8), (int)rd32(cpu.esp + 12),
              g_frame_n > 1 ? g_frames[g_frame_n - 1].ret_addr : 0);
    }
  }
  uint32_t start = cpu.eip;
  hist_add(start);
  g_insn_addr = start;
  if (g_watch_eip && start == g_watch_eip && !g_watch_seen) {
    g_watch_seen = 1;
    watch_record();
    if (g_watch_left > 0) {
      --g_watch_left;
      fprintf(stderr,
              "[emu] WATCH #%d %08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x "
              "edi=%08x ebp=%08x esp=%08x\n",
              ++g_watch_hits, start, cpu.eax, cpu.ebx, cpu.ecx, cpu.edx, cpu.esi,
              cpu.edi, cpu.ebp, cpu.esp);
      dump_words("  [edi-0x10]", cpu.edi - 0x10u, 12);
      dump_words("  [esp]", cpu.esp, 6);
    }
  }
  uint8_t op = imm8();
  int seg_fs = 0;
  int opsize16 = 0;
  g_seg_fs = 0;

  /* 段前缀 26/2e/36/3e/64/65、操作数/地址前缀 66/67：忽略（这个 DLL 不用） */
  while (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0x64 ||
         op == 0x65 || op == 0x66 || op == 0x67 || op == 0xf2 || op == 0xf3) {
    if (op == 0xf3) { /* rep：先只支持 rep stosl/movsl 的最小形态 */
      uint8_t n = imm8();
      const int df = (cpu.eflags & DF) ? -1 : 1;
      switch (n) {
        case 0xab: while (cpu.ecx--) { wr32(cpu.edi, cpu.eax); cpu.edi += 4 * df; } return 0; /* rep stosl */
        case 0xaa: while (cpu.ecx--) { wr8(cpu.edi, (uint8_t)cpu.eax); cpu.edi += df; } return 0; /* rep stosb */
        case 0xa5: while (cpu.ecx--) { wr32(cpu.edi, rd32(cpu.esi)); cpu.esi += 4 * df; cpu.edi += 4 * df; } return 0;
        case 0xa4: while (cpu.ecx--) { wr8(cpu.edi, rd8(cpu.esi)); cpu.esi += df; cpu.edi += df; } return 0;
        default: UNIMPL("rep ...");
      }
    }
    if (op == 0x64) { seg_fs = 1; g_seg_fs = 1; }  /* fs: → TIB 影子区 */
    if (op == 0x66) opsize16 = 1; /* 操作数 16 位（SYSTEMTIME 那套用 16 位字段） */
    op = imm8();
  }
  (void)start;

  if (op >= 0x50 && op <= 0x57) { push32(*reg32(op - 0x50)); return 0; }
  if (op >= 0x58 && op <= 0x5f) { *reg32(op - 0x58) = pop32(); return 0; }
  /* inc/dec r32：**必须把结果写回寄存器**！之前只算了标志没写回，
   * 导致模式号 `decl` 后仍是原值 → switch 跳转表索引整体偏移一格
   * （真机表现：idx8 的 case 1 走进了 case 2 的 body）。 */
  if (op >= 0x40 && op <= 0x47) {
    uint32_t *r = reg32(op - 0x40);
    alu_add(*r, 1);
    *r = *r + 1;
    return 0;
  }
  if (op >= 0x48 && op <= 0x4f) {
    uint32_t *r = reg32(op - 0x48);
    alu_sub(*r, 1);
    *r = *r - 1;
    return 0;
  }
  if (op >= 0x68 && op <= 0x68) { push32(imm32()); return 0; }
  if (op >= 0x6a && op <= 0x6a) { push32((uint32_t)(int32_t)rel8()); return 0; }
  if (op >= 0x70 && op <= 0x7f) { int8_t r = rel8(); if (cond(op - 0x70)) cpu.eip += r; return 0; }
  if (op >= 0xb8 && op <= 0xbf) { *reg32(op - 0xb8) = imm32(); return 0; }
  if (op >= 0xb0 && op <= 0xb7) { *reg8(op - 0xb0) = imm8(); return 0; }
  if (op >= 0x91 && op <= 0x97) { uint32_t *r = reg32(op - 0x90); uint32_t t = cpu.eax; cpu.eax = *r; *r = t; return 0; }
  /* AL/EAX, imm 族： (op&7)==4 是 AL+imm8，==5 是 EAX+imm32；高位选操作 */
  if (op <= 0x3d && ((op & 7) == 4 || (op & 7) == 5)) {
    int sub = (op >> 3) & 7;
    int is8 = (op & 7) == 4;
    uint32_t im = is8 ? (uint32_t)(int32_t)(int8_t)imm8() : imm32();
    uint32_t a = is8 ? (cpu.eax & 0xffu) : cpu.eax;
    uint32_t r;
    switch (sub) {
      case 0: r = a + im; alu_add(a, im); break;
      case 1: r = a | im; set_szp32(r); break;
      case 4: r = a & im; set_szp32(r); break;
      case 5: r = a - im; alu_sub(a, im); break;
      case 6: r = a ^ im; set_szp32(r); break;
      case 7: alu_cmp(a, im); return 0;
      default: UNIMPL("alu eax,imm (adc/sbb)");
    }
    if (is8) cpu.eax = (cpu.eax & 0xffffff00u) | (r & 0xffu);
    else cpu.eax = r;
    return 0;
  }

  switch (op) {
    case 0x90: return 0;                    /* nop */
    case 0x98: /* cwtl */ cpu.eax = (uint32_t)(int32_t)(int16_t)cpu.eax; return 0;
    case 0x99: /* cltd */ cpu.edx = (cpu.eax & 0x80000000u) ? 0xffffffffu : 0; return 0;
    /* mov al/eax <-> moffs（CRT 的 `mov eax, fs:[0]` 走这里；fs 一律按 0 处理） */
    case 0xa0: { uint32_t a = imm32() + (seg_fs ? TIB_BASE : 0); *reg8(0) = rd8(a); return 0; }
    case 0xa1: { uint32_t a = imm32() + (seg_fs ? TIB_BASE : 0); cpu.eax = rd32(a); return 0; }
    case 0xa2: { uint32_t a = imm32() + (seg_fs ? TIB_BASE : 0); wr8(a, *reg8(0)); return 0; }
    case 0xa3: { uint32_t a = imm32() + (seg_fs ? TIB_BASE : 0); wr32(a, cpu.eax); return 0; }
    case 0x9c: push32(cpu.eflags); return 0;
    case 0x9d: cpu.eflags = pop32(); return 0;
    case 0x89: {
      ModRM m = modrm();
      if (opsize16) {
        uint16_t v = (uint16_t)(*reg32(m.reg) & 0xffffu);
        if (m.is_reg) *reg32(m.addr) = (*reg32(m.addr) & 0xffff0000u) | v;
        else wr16(m.addr, v);
      } else rm_write32(m, *reg32(m.reg));
      return 0;
    }
    case 0x8b: {
      ModRM m = modrm();
      if (opsize16) {
        uint16_t v = m.is_reg ? (uint16_t)(*reg32(m.addr) & 0xffffu) : rd16(m.addr);
        *reg32(m.reg) = v;
      } else *reg32(m.reg) = rm_read32(m);
      return 0;
    }
    case 0x3b: { /* cmp r32, r/m32 */
      ModRM m = modrm();
      /* 注意：寄存器形式以前被无条件截断成低 16 位（只有 opsize16 才该截断）。
       * 后果是 `cmp %ebx,%eax` 在两边都是堆指针时永远「不等」——DLL 里 vector 扩容的
       * 前缀拷贝 `for (p = begin; p != pos; p += 4)` 就此变成死循环（真机卡小游戏
       * 的根因，200M 步上限每次都在 0x10004932 打转）。 */
      uint32_t b = m.is_reg ? *reg32(m.addr) : (opsize16 ? rd16(m.addr) : rd32(m.addr));
      if (opsize16) b &= 0xffffu;
      uint32_t a = *reg32(m.reg);
      if (opsize16) {
        uint32_t r = (a & 0xffffu) - b;
        cpu.eflags &= ~(CF | OF);
        if ((a & 0xffffu) < b) cpu.eflags |= CF;
        cpu.eflags &= ~(ZF | SF);
        if ((r & 0xffffu) == 0) cpu.eflags |= ZF;
        if (r & 0x8000u) cpu.eflags |= SF;
      } else {
        alu_cmp(a, b);
      }
      return 0;
    }
    /* "r32, r/m32" 方向的 ALU 族（0x31/0x39 是反方向，已实现） */
    case 0x03: case 0x0b: case 0x1b: case 0x23: case 0x2b: case 0x33: {
      ModRM m = modrm();
      uint32_t a = *reg32(m.reg);
      uint32_t b = m.is_reg ? *reg32(m.addr) : rd32(m.addr);
      switch (op) {
        case 0x03: alu_add(a, b); *reg32(m.reg) = a + b; break;
        case 0x0b: *reg32(m.reg) = a | b; set_szp32(a | b); break;
        case 0x1b: *reg32(m.reg) = alu_sbb(a, b); break; /* sbb r32, r/m32 */
        case 0x23: *reg32(m.reg) = a & b; set_szp32(a & b); break;
        case 0x2b: alu_sub(a, b); *reg32(m.reg) = a - b; break;
        default:   *reg32(m.reg) = a ^ b; set_szp32(a ^ b); break;
      }
      return 0;
    }
    case 0x88: { ModRM m = modrm(); rm_write8(m, *reg8(m.reg)); return 0; }
    case 0xa8: { uint32_t v = imm8(); cpu.eflags &= ~(CF | OF); set_szp8((uint8_t)cpu.eax & (uint8_t)v); return 0; } /* test al, imm8 */
    case 0xa9: { uint32_t v = imm32(); cpu.eflags &= ~(CF | OF); set_szp32(cpu.eax & v); return 0; }               /* test eax, imm32 */
    case 0x8a: { ModRM m = modrm(); *reg8(m.reg) = rm_read8(m); return 0; }
    case 0x8d: { ModRM m = modrm(); *reg32(m.reg) = m.addr; return 0; }   /* lea */
    case 0x8f: { ModRM m = modrm(); rm_write32(m, pop32()); return 0; }   /* pop r/m */
    case 0xff: {
      ModRM m = modrm();
      switch (m.reg) {
        case 0: case 1: { /* inc/dec r/m32 */
          uint32_t v = rm_read32(m);
          if (m.reg == 0) alu_add(v, 1); else alu_sub(v, 1);
          rm_write32(m, m.reg == 0 ? v + 1 : v - 1);
          return 0;
        }
        case 2: { /* call r/m32 */
          uint32_t t = rm_read32(m);
          uint32_t ret = cpu.eip;
          push32(ret);
          emu_call_push(t, ret, cpu.esp);
          cpu.eip = t;
          return 0;
        }
        case 4: { uint32_t t = rm_read32(m); cpu.eip = t; return 0; }                 /* jmp */
        case 6: push32(rm_read32(m)); return 0;                                        /* push */
        default: UNIMPL("ff /x");
      }
    }
    /* 8 位 ALU：0x00+(sub<<3) 是 r/m8, r8；0x02+(sub<<3) 是 r8, r/m8。
     * （编译器用 `xorb %bl,%bl` 之类清零，很常见。） */
    case 0x00: case 0x08: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0x02: case 0x0a: case 0x22: case 0x2a: case 0x32: case 0x3a: {
      ModRM m = modrm();
      uint8_t a = (op & 2) ? *reg8(m.reg) : rm_read8(m);
      uint8_t b = (op & 2) ? rm_read8(m) : *reg8(m.reg);
      uint8_t r;
      switch ((op >> 3) & 7) {
        case 0: r = (uint8_t)(a + b); set_szp8(r); break;
        case 1: r = (uint8_t)(a | b); set_szp8(r); break;
        case 4: r = (uint8_t)(a & b); set_szp8(r); break;
        case 5: r = (uint8_t)(a - b); set_szp8(r); break;
        case 6: r = (uint8_t)(a ^ b); set_szp8(r); break;
        default: /* cmp */ { uint8_t s = (uint8_t)(a - b); set_szp8(s); } return 0;
      }
      if (op & 2) *reg8(m.reg) = r; else rm_write8(m, r);
      return 0;
    }
    /* 0xF7 组：test/not/neg/mul/imul/div/idiv（单操作数） */
    /* 0xF6 族：8 位 test/not/neg/mul/imul/div/idiv。
     * 实体类型 1 起的 idx7 路径里有 `negb %al`（f6 d8）——整族之前没实现，
     * 执行器在这里直接报「未实现指令」返回，表现就是 71(1) 起 ret 留着残值、intD 一处不写。 */
    case 0xf6: {
      ModRM m = modrm();
      uint8_t v = rm_read8(m);
      switch (m.reg) {
        case 0: case 1: cpu.eflags &= ~(CF | OF); set_szp8(v & imm8()); return 0;
        case 2: rm_write8(m, (uint8_t)~v); return 0;                     /* not */
        case 3: {                                                        /* neg */
          uint8_t r8 = (uint8_t)(0 - v);
          cpu.eflags &= ~(CF | OF);
          if (v != 0) cpu.eflags |= CF;
          if (v == 0x80) cpu.eflags |= OF;
          set_szp8(r8);
          rm_write8(m, r8);
          return 0;
        }
        case 4: { uint16_t r16 = (uint16_t)((uint8_t)cpu.eax * v);  /* mul: AL*r/m8 -> AX */
                  cpu.eax = (cpu.eax & 0xffff0000u) | r16;
                  cpu.eflags &= ~(CF | OF);
                  if (r16 >> 8) cpu.eflags |= (CF | OF);
                  return 0; }
        case 5: { int16_t r16 = (int16_t)((int8_t)cpu.eax * (int8_t)v);  /* imul */
                  cpu.eax = (cpu.eax & 0xffff0000u) | (uint16_t)r16;
                  cpu.eflags &= ~(CF | OF);
                  if (r16 != (int16_t)(int8_t)r16) cpu.eflags |= (CF | OF);
                  return 0; }
        case 6: { if (v == 0) { fprintf(stderr, "[emu] div8 0\n"); return -1; }
                  uint16_t ax = (uint16_t)(cpu.eax & 0xffffu);
                  *reg8(0) = (uint8_t)(ax / v);
                  *reg8(4) = (uint8_t)(ax % v);
                  return 0; }
        case 7: { if (v == 0) { fprintf(stderr, "[emu] idiv8 0\n"); return -1; }
                  int16_t ax = (int16_t)(cpu.eax & 0xffffu);
                  *reg8(0) = (uint8_t)(ax / (int8_t)v);
                  *reg8(4) = (uint8_t)(ax % (int8_t)v);
                  return 0; }
      }
      UNIMPL("f6 /x");
    }
    case 0xf7: {
      ModRM m = modrm();
      uint32_t v = rm_read32(m);
      switch (m.reg) {
        case 0: cpu.eflags &= ~(CF | OF); set_szp32(v & cpu.eax); return 0; /* test */
        case 2: rm_write32(m, ~v); return 0;                                  /* not */
        case 3: alu_sub(0, v); rm_write32(m, (uint32_t)(-(int32_t)v)); return 0; /* neg */
        case 4: { uint64_t r = (uint64_t)cpu.eax * v; cpu.eax = (uint32_t)r; cpu.edx = (uint32_t)(r >> 32); return 0; }
        case 5: { int64_t r = (int64_t)(int32_t)cpu.eax * (int64_t)(int32_t)v;
                  cpu.eax = (uint32_t)r; cpu.edx = (uint32_t)(r >> 32); return 0; }
        case 6: { if (v == 0) { fprintf(stderr, "[emu] div 0\n"); return -1; }
                  /* div 的被除数是 EDX:EAX（64 位）；只看 EAX 会在 edx≠0 时算错 */
                  uint64_t dv = ((uint64_t)cpu.edx << 32) | cpu.eax;
                  uint32_t q = (uint32_t)(dv / v), r2 = (uint32_t)(dv % v);
                  cpu.eax = q; cpu.edx = r2; return 0; }
        case 7: { if (v == 0) { fprintf(stderr, "[emu] idiv 0\n"); return -1; }
                  int64_t dv = (int64_t)(((uint64_t)cpu.edx << 32) | cpu.eax);
                  int64_t dvs = (int32_t)v; /* 除数按有符号扩展 */
                  int32_t q = (int32_t)(dv / dvs), r2 = (int32_t)(dv % dvs);
                  cpu.eax = (uint32_t)q; cpu.edx = (uint32_t)r2; return 0; }
      }
      UNIMPL("f7 /x");
    }
    /* 0x84：test r/m8, r8（0x85 是 32 位；实体类型 1 起的 idx7 路径用它做返回值判断） */
    case 0x84: {
      ModRM m = modrm();
      cpu.eflags &= ~(CF | OF);
      set_szp8(rm_read8(m) & *reg8(m.reg));
      return 0;
    }
    case 0x01: case 0x19: case 0x29: case 0x31: case 0x09: case 0x21: case 0x39: case 0x85: {
      ModRM m = modrm();
      uint32_t a = rm_read32(m), b = *reg32(m.reg);
      switch (op) {
        case 0x01: alu_add(a, b); rm_write32(m, a + b); break;
        case 0x19: rm_write32(m, alu_sbb(a, b)); break; /* sbb r/m32, r32 */
        case 0x29: alu_sub(a, b); rm_write32(m, a - b); break;
        case 0x31: rm_write32(m, a ^ b); set_szp32(a ^ b); break;
        case 0x09: rm_write32(m, a | b); set_szp32(a | b); break;
        case 0x21: rm_write32(m, a & b); set_szp32(a & b); break;
        case 0x39: alu_cmp(a, b); break;
        default:   cpu.eflags &= ~(CF | OF); set_szp32(a & b); break; /* test */
      }
      return 0;
    }
    case 0x83: { /* alu r/m32, imm8 */
      ModRM m = modrm();
      int32_t im = rel8();
      uint32_t a = rm_read32(m);
      switch (m.reg) {
        case 0: alu_add(a, im); rm_write32(m, a + im); break;
        case 1: rm_write32(m, a | im); set_szp32(a | im); break;
        case 4: rm_write32(m, a & im); set_szp32(a & im); break;
        case 5: alu_sub(a, im); rm_write32(m, a - im); break;
        case 6: rm_write32(m, a ^ im); set_szp32(a ^ im); break;
        case 7: alu_cmp(a, im); break;
        default: UNIMPL("83 /x");
      }
      return 0;
    }
    case 0x81: { /* alu r/m32, imm32 */
      ModRM m = modrm();
      uint32_t im = imm32();
      uint32_t a = rm_read32(m);
      switch (m.reg) {
        case 0: alu_add(a, im); rm_write32(m, a + im); break;
        case 5: alu_sub(a, im); rm_write32(m, a - im); break;
        case 7: alu_cmp(a, im); break;
        case 1: rm_write32(m, a | im); set_szp32(a | im); break;
        case 4: rm_write32(m, a & im); set_szp32(a & im); break;
        case 6: rm_write32(m, a ^ im); set_szp32(a ^ im); break;
        default: UNIMPL("81 /x");
      }
      return 0;
    }
    case 0x80: case 0x82: { /* ALU r/m8, imm8（`orb $0xc,%ah` 就是 _ftol2 里的那条） */
      ModRM m = modrm();
      uint8_t im = imm8();
      uint8_t a = rm_read8(m);
      uint8_t r;
      switch (m.reg) {
        case 0: r = (uint8_t)(a + im); set_szp8(r); break;
        case 1: r = (uint8_t)(a | im); set_szp8(r); break;
        case 4: r = (uint8_t)(a & im); set_szp8(r); break;
        case 5: r = (uint8_t)(a - im); set_szp8(r); break;
        case 6: r = (uint8_t)(a ^ im); set_szp8(r); break;
        case 7: { uint8_t s = (uint8_t)(a - im); set_szp8(s); } return 0;
        default: UNIMPL("80 /x");
      }
      rm_write8(m, r);
      return 0;
    }
    case 0xc1: case 0xd1: case 0xd3: { /* 移位 */
      ModRM m = modrm();
      int cnt = (op == 0xc1) ? imm8() : (op == 0xd1 ? 1 : (int)(cpu.ecx & 31));
      uint32_t v = rm_read32(m);
      uint32_t r;
      switch (m.reg) {
        case 4: r = v << cnt; break;
        case 5: r = v >> cnt; break;
        case 7: r = (uint32_t)((int32_t)v >> cnt); break;
        default: UNIMPL("c1/d1/d3 /x");
      }
      rm_write32(m, r);
      set_szp32(r);
      return 0;
    }
    case 0xe8: { /* call rel32 */
      int32_t r = (int32_t)imm32();
      uint32_t ret = cpu.eip;
      push32(ret);
      emu_call_push(ret + r, ret, cpu.esp);
      cpu.eip = ret + r;
      return 0;
    }
    case 0xe9: { int32_t r = (int32_t)imm32(); cpu.eip += r; return 0; }
    case 0xeb: { int8_t r = rel8(); cpu.eip += r; return 0; }
    case 0xc3: {
      uint32_t t = rd32(cpu.esp);
      if (emu_ret_check() != 0) return -1;
      cpu.eip = t;
      return 0;
    }
    case 0xc2: { /* ret imm16：stdcall 的返回并清参数 */
      uint16_t n = rd16(cpu.eip);
      cpu.eip += 2;
      uint32_t t = rd32(cpu.esp);
      if (emu_ret_check() != 0) return -1;
      cpu.eip = t;
      cpu.esp += n;
      return 0;
    }
    case 0xc9: cpu.esp = cpu.ebp; cpu.ebp = pop32(); return 0; /* leave */
    case 0xc7: { ModRM m = modrm(); rm_write32(m, imm32()); return 0; }
    case 0xc6: { ModRM m = modrm(); rm_write8(m, imm8()); return 0; }
    case 0x69: case 0x6b: { /* imul r, r/m, imm */
      ModRM m = modrm();
      int32_t im = (op == 0x69) ? (int32_t)imm32() : (int32_t)rel8();
      *reg32(m.reg) = (uint32_t)((int32_t)rm_read32(m) * im);
      return 0;
    }
    case 0x0f: {
      uint8_t op2 = imm8();
      if (op2 >= 0x80 && op2 <= 0x8f) { int32_t r = (int32_t)imm32(); if (cond(op2 - 0x80)) cpu.eip += r; return 0; }
      if (op2 == 0xaf) { ModRM m = modrm(); *reg32(m.reg) = (uint32_t)((int32_t)rm_read32(m) * (int32_t)*reg32(m.reg)); return 0; }
      if (op2 == 0xb6) { ModRM m = modrm(); *reg32(m.reg) = rm_read8(m); return 0; }   /* movzbl */
      if (op2 == 0xb7) { ModRM m = modrm(); *reg32(m.reg) = rd8(m.addr) | (rd8(m.addr + 1) << 8); return 0; }
      if (op2 == 0xbe) { ModRM m = modrm(); *reg32(m.reg) = (uint32_t)(int32_t)(int8_t)rm_read8(m); return 0; }
      if (op2 >= 0x90 && op2 <= 0x9f) { ModRM m = modrm(); rm_write8(m, cond(op2 - 0x90) ? 1 : 0); return 0; }
      if (op2 == 0x05) { fprintf(stderr, "[emu] syscall/nop 形式未实现\n"); return -1; }
      UNIMPL("0f xx");
    }
    case 0xcc: fprintf(stderr, "[emu] int3 命中（不该被执行到）\n"); return -1;
    /* 无 rep 前缀的串指令（编译器把 memcpy/memset 内联时的形态） */
    case 0xab: wr32(cpu.edi, cpu.eax); cpu.edi += 4; return 0;   /* stosl */
    case 0xaa: wr8(cpu.edi, (uint8_t)cpu.eax); cpu.edi += 1; return 0; /* stosb */
    case 0xa5: wr32(cpu.edi, rd32(cpu.esi)); cpu.esi += 4; cpu.edi += 4; return 0; /* movsl */
    case 0xa4: wr8(cpu.edi, rd8(cpu.esi)); cpu.esi += 1; cpu.edi += 1; return 0;   /* movsb */
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
      if (x87(op) == 0) return 0;
      cpu.eip = start; /* 回退 eip，让报错指向指令开头 */
      UNIMPL("x87");
    case 0x9b: return 0; /* fwait，忽略 */
    default: UNIMPL("opcode");
  }
}

/* ------------------------------------------------------- PE 装载（最小） */
static int load_pe_bytes(const uint8_t *buf, long sz);

/* CLI 包装：读文件 → 内存装载 → 释放 */
static int load_pe(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "打不开 %s\n", path); return -1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return -1; }
  uint8_t *buf = (uint8_t *)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
  fclose(f);
  int rc = load_pe_bytes(buf, sz);
  free(buf);
  return rc;
}

/* 从内存里的 PE 映像装载（Android 侧不方便给路径；CLI 用下面的 load_pe 包一层）。
 * 注意：opt/sec 都指向 buf，所以 buf 必须活到函数结束（原来 free 早了，属侥幸能跑）。 */
static int load_pe_bytes(const uint8_t *buf, long sz) {
  (void)sz;
  uint32_t pe = *(uint32_t *)(buf + 0x3c);
  if (memcmp(buf + pe, "PE\0\0", 4) != 0) { fprintf(stderr, "不是 PE\n"); return -1; }
  uint16_t nsec = *(uint16_t *)(buf + pe + 6);
  uint16_t optsz = *(uint16_t *)(buf + pe + 20);
  const uint8_t *opt = buf + pe + 24;
  uint32_t entry = *(uint32_t *)(opt + 16);
  const uint8_t *sec = opt + optsz;
  for (int i = 0; i < nsec; ++i, sec += 40) {
    uint32_t vsize = *(uint32_t *)(sec + 8), vaddr = *(uint32_t *)(sec + 12);
    uint32_t rawsz = *(uint32_t *)(sec + 16), rawptr = *(uint32_t *)(sec + 20);
    uint32_t va = IMAGE_BASE + vaddr;
    if (!in_guest(va, vsize ? vsize : rawsz)) { fprintf(stderr, "段越界\n"); return -1; }
    memcpy(gp(va), buf + rawptr, rawsz);
    memset(gp(va) + rawsz, 0, (vsize > rawsz ? vsize - rawsz : 0));
  }
  /* buf 由调用者负责释放：CLI 的 load_pe 会在返回后 free，库模式是调用者的缓冲区 */
  /* 解析导入目录：给每个导入函数分配桩地址，写进 IAT */
  uint32_t dd_import = *(uint32_t *)(opt + 96 + 1 * 8);
  if (dd_import && g_stub_count == 0) {
    uint8_t *d = gp(IMAGE_BASE + dd_import);
    for (int k = 0; k < 32; ++k) {
      uint32_t oft = *(uint32_t *)(d + k * 20 + 0);
      uint32_t namerva = *(uint32_t *)(d + k * 20 + 12);
      uint32_t firstthunk = *(uint32_t *)(d + k * 20 + 16);
      if (!namerva && !firstthunk) break;
      uint32_t ilt = oft ? oft : firstthunk;
      for (int j = 0; j < 512; ++j) {
        uint32_t ent = *(uint32_t *)(gp(IMAGE_BASE + ilt) + j * 4);
        if (!ent) break;
        uint32_t iat = IMAGE_BASE + firstthunk + j * 4;
        if ((ent & 0x80000000u) == 0) {
          const char *fn = (const char *)gp(IMAGE_BASE + ent + 2);
          snprintf(g_stub_name[g_stub_count], sizeof(g_stub_name[0]), "%s", fn);
        } else {
          snprintf(g_stub_name[g_stub_count], sizeof(g_stub_name[0]), "ord#%u",
                   ent & 0xffffu);
        }
        wr32(iat, STUB_BASE + (uint32_t)g_stub_count * STUB_STRIDE);
        printf("# import[%d] %s -> IAT %08x\n", g_stub_count,
               g_stub_name[g_stub_count], iat);
        if (++g_stub_count >= STUB_MAX) break;
      }
    }
  }
  return (int)entry;
}

/* ------------------------------------------------------------------ main */
#ifdef PT00_EMU_LIBRARY
/* ======================= 库模式（Android app 用） =======================
 * 与 CLI 共用同一个解释器，只把「装载 / 调用 / 取结果」暴露成函数。
 * 接口约定：intD 是唯一的共享内存，调用前后由平台层搬运：
 *   pt00_emu_load_image(bytes, size);       // PE 装载 + func_load + func_init
 *   pt00_emu_set_intd(engine_intd, 2000);   // 调用前：引擎 → 执行器
 *   pt00_emu_call(func, a1, a2, a3, a4);
 *   pt00_emu_get_intd(engine_intd, 2000);   // 调用后：执行器 → 引擎
 * 原版 PT00.dll 由用户游戏数据在运行时提供，不进 APK。 */
static int emu_run(uint32_t entry_, int argc_, const uint32_t *args) {
  /* 每次进入都是一次独立的 CallDLL：**必须把调用栈帧清零**。
   * 上一次调用若是「步数上限/栈不平衡」提前返回（没有逐层 ret），g_frame_n 会留在
   * 高位；再叠加几次就会超过 EMU_CALLDEPTH，然后「打印调用栈」的循环越界 —— 报错路径
   * 自己 SIGSEGV（真机运镜时就是这么崩的）。 */
  g_frame_n = 0;
  g_steps = 0;
  g_last_hit_step_limit = 0;
  g_watch_seen = 0;
  uint32_t esp0 = cpu.esp;
  for (int i = argc_ - 1; i >= 0; --i) push32(args[i]);
  push32(SENTINEL);
  emu_call_push(entry_, SENTINEL, cpu.esp);
  cpu.eip = entry_;
  int rc = -1;
  for (;;) {
    if (step() != 0) break;
    if (cpu.eip == SENTINEL) { rc = 0; break; }
    if (!(cpu.eip >= IMAGE_BASE && cpu.eip < IMAGE_BASE + 0x30000u) &&
        !(cpu.eip >= STUB_BASE && cpu.eip < STUB_BASE + STUB_MAX * STUB_STRIDE))
      break;
  }
  /* 栈必须回到调用前。入口 reallive_dll_func_call 本身是 stdcall（每条出口都是
   * `ret 0x14`，已经清掉 5 个参数），再补一次 argc*4 会让 esp 每个 CallDLL 漂
   * 20 字节 —— 真机实测 8963 次调用后 esp 从 0x10200000 漂到 0x1022bc40。 */
  if (rc == 0) {
    if (cpu.esp != esp0) {
      static int drift_logged = 0;
      if (drift_logged++ < 10)
        fprintf(stderr,
                "[emu] 调用前后 esp 不平衡：%08x -> %08x（差 %d 字节），已强制复位\n",
                esp0, cpu.esp, (int)(cpu.esp - esp0));
    }
    cpu.esp = esp0;
  }
  return rc;
}

int pt00_emu_load_image(const void *data, unsigned size) {
  emu_install_fault_handler();
  if (!g_mem) {
    g_mem = (uint8_t *)calloc(1, GUEST_SIZE);
    if (!g_mem) return -1;
  }
  memset(g_mem, 0, GUEST_SIZE);
  memset(&cpu, 0, sizeof(cpu));
  memset(g_tls_val, 0, sizeof(g_tls_val));
  memset(g_tls_used, 0, sizeof(g_tls_used));
  memset(g_stub_hits, 0, sizeof(g_stub_hits));
  g_stub_count = 0;
  g_heap_ptr = 0x11400000u;
  g_frame_n = 0;
  g_steps = 0;
  g_last_rc = -1;
  int entry = load_pe_bytes((const uint8_t *)data, (long)size);
  if (entry < 0) return -2;
  cpu.esp = 0x10200000u;
  cpu.ebp = cpu.esp;
  wr32(TIB_BASE + 0x18, TIB_BASE);
  wr32(TIB_BASE + 0x00, 0xffffffffu);
  wr32(CTX_BASE + 0x14, INTD_BASE);
  uint32_t args[5] = {CTX_BASE, 0, 0, 0, 0};
  if (emu_run(IMAGE_BASE + 0x11B0u, 2, args) != 0) return -3; /* func_load */
  { /* 与 oracle 对齐：钉死 CRT 的 rand 起点（对照装置语义，见 HANDOFF §7.2） */
    uint32_t idx = rd32(IMAGE_BASE + 0x20AA0);
    uint32_t ptd = g_tls_val[tls_slot(idx)];
    if (ptd) wr32(ptd + 0x14, 1);
  }
  emu_run(IMAGE_BASE + 0x1670u, 0, args); /* func_init */
  return 0;
}

int pt00_emu_call(int func, int a1, int a2, int a3, int a4) {
  uint32_t args[5] = {(uint32_t)func, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                      (uint32_t)a4};
  if (emu_run(IMAGE_BASE + 0x1680u, 5, args) != 0) return 1; /* 契约：恒返回 1 */
  return (int)cpu.eax;
}

/* 上一次调用是不是撞了步数上限（DLL 死循环）。见 g_last_hit_step_limit 注释。 */
int pt00_emu_last_hit_step_limit(void) { return g_last_hit_step_limit; }

/* 假堆已用字节数（诊断：排查「DLL 内部数据结构被踩」时看堆增长是否异常）。 */
unsigned pt00_emu_heap_used(void) { return (unsigned)(g_heap_ptr - 0x11400000u); }

void pt00_emu_set_intd(const int *src, unsigned count) {
  for (unsigned i = 0; i < count; ++i) wr32(INTD_BASE + i * 4u, (uint32_t)src[i]);
}

/* 取证开关：记录 DLL 对 ctx 块（+0x00..+0xFF）与低地址的读写，最多 n 条。
 * 用途：小游戏卡住时判断 DLL 是不是在找**别的引擎数组**（比如 intG）。 */
void pt00_emu_set_trace_ctx(int on, int max_lines) {
  g_trace_ctx = on;
  g_ctx_log_left = max_lines > 0 ? max_lines : 400;
}

void pt00_emu_get_intd(int *dst, unsigned count) {
  for (unsigned i = 0; i < count; ++i) dst[i] = (int)rd32(INTD_BASE + i * 4u);
}
#else
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: emu <PT00.dll> [calls.txt] [--seed seed.bin]\n");
    return 2;
  }
  g_mem = (uint8_t *)calloc(1, GUEST_SIZE);
  if (!g_mem) return 1;

  int entry = load_pe(argv[1]);
  if (entry < 0) return 1;
  printf("# 装载 %s，入口 %08x（镜像基址 %08x）\n", argv[1], entry, IMAGE_BASE);

  /* 最小栈：镜像之后、intD 之前（镜像约在 0x10001000..0x10029000） */
  cpu.esp = 0x10200000u;
  cpu.ebp = cpu.esp;
  cpu.eip = IMAGE_BASE + 0x11B0; /* reallive_dll_func_load（阶段 1 直接从这里起跑） */

  /* TIB 影子：fs:[0x18] 存 TIB 自身地址；fs:[0]（SEH 链头）初始为 -1（无处理器） */
  wr32(TIB_BASE + 0x18, TIB_BASE);
  wr32(TIB_BASE + 0x00, 0xffffffffu);
  /* 引擎上下文：+0x14 处放 intD 基址（这是 PT00 唯一用到的约定） */
  wr32(CTX_BASE + 0x14, INTD_BASE);

  /* --seed：从二进制快照恢复 intD[2000]+intF[2000]（与 oracle 同格式）。
   * **必须两边同种子**，否则起点不同、对照就是噪声（例如 emu 从全 0 起跑时
   * 实体记录 record[0]=0，func 71 第一句就 return，表现为"什么都不写"）。 */
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--trace-ctx") == 0) {
      g_trace_ctx = 1;
      g_ctx_log_left = 4000;
    }
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      FILE *sf = fopen(argv[i + 1], "rb");
      if (!sf) { fprintf(stderr, "打不开种子 %s\n", argv[i + 1]); return 1; }
      for (int k = 0; k < INTD_COUNT; ++k) {
        int v = 0;
        if (fread(&v, sizeof(int), 1, sf) != 1) break;
        wr32(INTD_BASE + k * 4, (uint32_t)v);
      }
      for (int k = 0; k < INTF_COUNT; ++k) {
        int v = 0;
        if (fread(&v, sizeof(int), 1, sf) != 1) break;
        wr32(INTF_BASE + k * 4, (uint32_t)v);
      }
      fclose(sf);
      printf("# 种子已加载：%s（intD[70..76]=%d,%d,%d,%d,%d,%d,%d）\n", argv[i + 1],
             (int)rd32(INTD_BASE + 70 * 4), (int)rd32(INTD_BASE + 71 * 4),
             (int)rd32(INTD_BASE + 72 * 4), (int)rd32(INTD_BASE + 73 * 4),
             (int)rd32(INTD_BASE + 74 * 4), (int)rd32(INTD_BASE + 75 * 4),
             (int)rd32(INTD_BASE + 76 * 4));
    }
  }

  /* 跑一个导出：入口处压哨兵当返回地址，跑到哨兵即视为正常返回。
   * 参数按 stdcall 约定从右往左压，最上面是哨兵。 */
  #define RUN_EXPORT(entry, argc, ...)                              \
    do {                                                            \
      uint32_t a_[8] = {0, 0, 0, 0, 0, 0, 0, 0};                    \
      const uint32_t vals_[8] = {__VA_ARGS__};                      \
      for (int i_ = 0; i_ < (argc) && i_ < 8; ++i_) a_[i_] = vals_[i_]; \
      uint32_t esp0_ = cpu.esp;                                     \
      g_watch_seen = 0;                                             \
      for (int i_ = (argc) - 1; i_ >= 0; --i_) push32(a_[i_]);      \
      push32(SENTINEL);                                             \
      emu_call_push((entry), SENTINEL, cpu.esp); /* 最外层也登记一帧 */ \
      cpu.eip = (entry);                                            \
      int rc_ = -1;                                                 \
      for (;;) {                                                    \
        if (step() != 0) break;                                     \
        if (cpu.eip == SENTINEL) { rc_ = 0; break; }                 \
        if (!(cpu.eip >= IMAGE_BASE && cpu.eip < IMAGE_BASE + 0x30000u) && \
            !(cpu.eip >= STUB_BASE && cpu.eip < STUB_BASE + STUB_MAX * STUB_STRIDE)) { \
          fprintf(stderr, "[emu] eip=%08x 跑出镜像范围，停止\n", cpu.eip); \
          hist_dump();                                              \
          break;                                                    \
        }                                                           \
      }                                                             \
      /* 入口自己就是 stdcall（ret 0x14 已清参数），把 esp 复位到调用前即可。 */ \
      if (rc_ == 0) { cpu.esp = esp0_; }                            \
      g_last_rc = rc_;                                              \
    } while (0)

  printf("# 跑 func_load(%08x)\n", CTX_BASE);
  /* 注：跑 PE 入口（DllMainCRTStartup）会进入 CRT 自带的堆初始化，那套代码要靠
   * 真实 Windows 堆的布局/缺页才能收敛，在平坦 guest 空间里会死循环（已实测）。
   * 这里改为只补上"CRT per-thread 数据要能持久"这一条——见 tls_slot()。 */
  RUN_EXPORT(IMAGE_BASE + 0x11B0, 2, CTX_BASE, 0);
  printf("# func_load 结束 rc=%d（步数 %d）\n", g_last_rc, g_steps);
  if (g_last_rc != 0) return 1;
  /* 关键检查：func_load 的副作用落地了吗？
   *   dword_100237C4 = 22 个实体对象指针的数组（应当已填满堆指针）
   *   dword_10023D18 = 引擎上下文槽（应当 = 我们传进去的 CTX_BASE） */
  printf("# 检查 func_load 的副作用：\n");
  printf("#   pt00_dll_ctx[10023D18] = %08x（期望 %08x）\n",
         rd32(IMAGE_BASE + 0x23D18), CTX_BASE);
  printf("#   pt00_entities[100237C4] = %08x  [C8]=%08x  [CC]=%08x\n",
         rd32(IMAGE_BASE + 0x237C4), rd32(IMAGE_BASE + 0x237C8),
         rd32(IMAGE_BASE + 0x237CC));
  printf("#   堆已用 = %u 字节（起点 %08x 现在 %08x）\n",
         (unsigned)(g_heap_ptr - 0x11400000u), 0x11400000u, g_heap_ptr);
  printf("#   eax=%08x\n", cpu.eax);
  { /* CRT 的 rand 状态：ptd+0x14（ptd 由 TLS 槽给出）——与 oracle 的同类打印对齐 */
    uint32_t idx = rd32(IMAGE_BASE + 0x20AA0);
    int slot = tls_slot(idx);
    uint32_t ptd = g_tls_val[slot];
    /* 对照用：把 CRT 的 rand 起点钉死成同一个值（两边都钉），否则种子来源
     * （time() 由被桩掉的 GetSystemTime/GetLocalTime 推出来）会把随机序列带偏。 */
    if (ptd) wr32(ptd + 0x14, 1);
    printf("# TLS idx=%08x slot=%d ptd=%08x holdrand=%d\n", idx, slot, ptd,
           ptd ? (int)rd32(ptd + 0x14) : -1);
  }
  { /* 实体对象非零字段快照（挑实体 0/3/5；与 oracle 的同类打印对齐，
     * 用来判断 71(3)/71(5) 的分歧是不是"原生对象里有残留数据、我们是全 0"） */
    for (int i = 0; i < 22; ++i) {
      if (i != 0 && i != 3 && i != 5) continue;
      uint32_t p = rd32(IMAGE_BASE + 0x237C4 + 4 * i);
      printf("# ent[%d] @%08x:", i, p);
      for (int off = 0; off < 0x100; off += 4) {
        uint32_t v = rd32(p + off);
        if (v) printf(" +%02x=%08x", off, v);
      }
      printf("\n");
    }
  }

  printf("# 跑 func_init\n");
  RUN_EXPORT(IMAGE_BASE + 0x1670, 0);
  printf("# func_init 结束 rc=%d\n", g_last_rc);

  /* 指令自测（回归）：`cmp r32, r/m32` 的寄存器-寄存器形式必须用**完整 32 位**。
   * 以前 rm 侧被无条件截断成低 16 位，导致 DLL 里 `cmp %ebx,%eax; jne` 在两边
   * 都是堆指针时永远不等 —— vector 扩容的前缀拷贝就成了死循环（真机卡小游戏）。 */
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--selftest") != 0) continue;
    static const uint8_t kCmpCode[] = {
        0xB8, 0x64, 0x6E, 0x40, 0x11, /* mov eax,0x11406E64 */
        0xBB, 0x64, 0x6E, 0x40, 0x11, /* mov ebx,0x11406E64 */
        0x3B, 0xC3,                   /* cmp eax,ebx */
        0x75, 0x06,                   /* jne +6 → fail */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax,1（相等） */
        0xC3,                         /* ret */
        0xB8, 0x00, 0x00, 0x00, 0x00, /* mov eax,0（不等） */
        0xC3,                         /* ret */
    };
    const uint32_t at = IMAGE_BASE + 0x2F000u; /* 映像内、无 section 覆盖的零区 */
    for (size_t k = 0; k < sizeof(kCmpCode); ++k)
      wr8(at + (uint32_t)k, kCmpCode[k]);
    RUN_EXPORT(at, 0);
    printf("# selftest cmp r32,r/m32(reg,reg) 大值相等：eax=%d → %s\n",
           (int)cpu.eax, cpu.eax == 1 ? "PASS" : "FAIL（低 16 位截断回归）");
    /* x87 回归：FIADD m32int（真机日志里 0x100033ED 的 `da 46 4c` 就是这条）。
     * 4 + 4 后 fistp 回内存再读 eax，期望 8。 */
    static const uint8_t kX87Code[] = {
        0xC7, 0x05, 0x00, 0x01, 0x2F, 0x10, 0x04, 0x00, 0x00, 0x00, /* mov dword [0x1002F100],4 */
        0xDB, 0x05, 0x00, 0x01, 0x2F, 0x10,                         /* fild dword [0x1002F100] */
        0xDB, 0x05, 0x00, 0x01, 0x2F, 0x10,                         /* fild dword [0x1002F100] */
        0xDA, 0x05, 0x00, 0x01, 0x2F, 0x10,                         /* fiadd dword [0x1002F100] */
        0xDB, 0x1D, 0x00, 0x01, 0x2F, 0x10,                         /* fistp dword [0x1002F100] */
        0xA1, 0x00, 0x01, 0x2F, 0x10,                               /* mov eax,[0x1002F100] */
        0xC3,                                                       /* ret */
    };
    const uint32_t at2 = IMAGE_BASE + 0x2F200u;
    for (size_t k = 0; k < sizeof(kX87Code); ++k)
      wr8(at2 + (uint32_t)k, kX87Code[k]);
    RUN_EXPORT(at2, 0);
    printf("# selftest x87 fiadd m32int（4+4）：eax=%d → %s\n", (int)cpu.eax,
           cpu.eax == 8 ? "PASS" : "FAIL（m32int 组未实现或算错）");
  }

  if (argc > 2) {
    FILE *f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "打不开调用脚本 %s\n", argv[2]); return 1; }
    char line[512];
    int n = 0;
    int prev[INTD_COUNT];
    for (int i = 0; i < INTD_COUNT; ++i) prev[i] = rd32(INTD_BASE + i * 4);
    while (fgets(line, sizeof(line), f)) {
      char *p = line;
      while (*p == ' ' || *p == '\t') ++p;
      if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
      int v[5] = {0, 0, 0, 0, 0};
      int got = sscanf(p, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]);
      if (got < 1) continue;
      /* --trace：只把第一次调用全量打出来（默认关） */
      for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--trace") == 0 && n == 0) {
          g_trace = 1;
          g_trace_left = 400;
          break;
        }
        if (strcmp(argv[i], "--trace-func") == 0 && i + 1 < argc && !g_trace &&
            v[0] == atoi(argv[i + 1])) {
          g_trace = 1;
          g_trace_left = 40000;
          fprintf(stderr, "# trace func=%d\n", v[0]);
          break;
        }
        if (strcmp(argv[i], "--trace-min") == 0 && i + 1 < argc) {
          g_trace_min = (uint32_t)strtoul(argv[i + 1], NULL, 16);
        }
        if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc) {
          pt00_emu_set_watch((uint32_t)strtoul(argv[i + 1], NULL, 16), 400);
        }
        if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
          g_max_steps = atoi(argv[i + 1]);
        }
      }
      RUN_EXPORT(IMAGE_BASE + 0x1680, 5, (uint32_t)v[0], (uint32_t)v[1],
                 (uint32_t)v[2], (uint32_t)v[3], (uint32_t)v[4]);
      g_trace = 0;
      ++n;
      printf("%d func=%d(%d,%d,%d,%d) ret=%d", n, v[0], v[1], v[2], v[3], v[4],
             (int)cpu.eax);
      for (int i = 0; i < INTD_COUNT; ++i) {
        uint32_t cur = rd32(INTD_BASE + i * 4);
        if ((int)cur != prev[i]) { printf(" intD[%d]=%d", i, (int)cur); prev[i] = (int)cur; }
      }
      printf("\n");
    }
    fclose(f);
  }
  /* --dump <file>：跑完后把 intD[2000]+intF[2000] 整片写成二进制（与 oracle 的 out.bin 同格式），
   * 用来做「整片状态」对照——逐调用的差异行看不出没被写的槽位。 */
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
      FILE *o = fopen(argv[i + 1], "wb");
      if (!o) { fprintf(stderr, "打不开 %s\n", argv[i + 1]); return 1; }
      for (int k = 0; k < INTD_COUNT; ++k) { int v = (int)rd32(INTD_BASE + k * 4); fwrite(&v, sizeof(int), 1, o); }
      for (int k = 0; k < INTF_COUNT; ++k) { int v = (int)rd32(INTF_BASE + k * 4); fwrite(&v, sizeof(int), 1, o); }
      fclose(o);
      printf("# intD+intF 快照 -> %s\n", argv[i + 1]);
    } else if (strcmp(argv[i], "--dump-image") == 0 && i + 1 < argc) {
      /* 整个映像 0x30000 字节（含 .data 里被 pt00_prefix_sum_table 改过的表） */
      FILE *oi = fopen(argv[i + 1], "wb");
      if (!oi) { fprintf(stderr, "打不开 %s\n", argv[i + 1]); return 1; }
      fwrite(gp(IMAGE_BASE), 1, 0x28000u, oi);
      fclose(oi);
      printf("# 映像快照 -> %s\n", argv[i + 1]);
    }
  }
  printf("# 结束于 eip=%08x（步数 %d）\n", cpu.eip, g_steps);
  return 0;
}
#endif /* PT00_EMU_LIBRARY */
