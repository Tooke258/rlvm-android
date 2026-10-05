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

#define GUEST_SIZE (64u * 1024u * 1024u) /* 平坦 32 位空间的一部分，够这个 DLL 用 */
#define IMAGE_BASE 0x10000000u
#define INTD_COUNT 2000
#define INTF_COUNT 2000

/* guest 空间覆盖 [0x10000000, 0x10000000+GUEST_SIZE)——DLL 的镜像基址就在
 * 0x10000000，所以必须做一层基址翻译，不能直接从 0 起算。 */
#define GUEST_BASE 0x10000000u
static uint8_t *g_mem;

/* 导入桩：给每个 KERNEL32 导入函数分一个 guest 地址，写进 IAT。
 * 执行器在 step() 里拦截落在桩区间的 eip，改调宿主实现，然后按 cdecl ret。 */
#define STUB_BASE 0x20000000u
#define STUB_STRIDE 16u
#define STUB_MAX 256
static char g_stub_name[STUB_MAX][64];
static int g_stub_count = 0;
static int g_stub_hits[STUB_MAX];

static uint32_t g_heap_ptr = 0x11400000u; /* CRT 堆的假实现：bump allocator */
static uint32_t guest_alloc(uint32_t n) {
  uint32_t p = g_heap_ptr;
  g_heap_ptr += (n + 15u) & ~15u;
  return p;
}

static inline int in_guest(uint32_t a, uint32_t n) {
  if (a < GUEST_BASE) return 0;
  return (uint64_t)(a - GUEST_BASE) + n <= GUEST_SIZE;
}
static inline uint8_t *gp(uint32_t a) { return g_mem + (a - GUEST_BASE); }

static uint8_t rd8(uint32_t a) { return in_guest(a, 1) ? *gp(a) : 0; }
static uint32_t rd32(uint32_t a) {
  return in_guest(a, 4) ? *(uint32_t *)gp(a) : 0;
}
static void wr32(uint32_t a, uint32_t v) {
  if (in_guest(a, 4)) *(uint32_t *)gp(a) = v;
}
static void wr8(uint32_t a, uint8_t v) {
  if (in_guest(a, 1)) *gp(a) = v;
}

/* ------------------------------------------------------------------ CPU */
typedef struct {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t eip;
  uint32_t eflags;
  uint8_t *fpregs[8]; /* 阶段 2 用：x87 栈 */
  int fptop;
} CPU;

static CPU cpu;

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

typedef struct { int mod, reg, rm; uint32_t addr; int is_reg; } ModRM;

/* 计算 ModRM，返回操作数地址（is_reg=1 时 addr 是寄存器编号） */
static ModRM modrm(void) {
  ModRM m;
  uint8_t b = imm8();
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
      if (bs == 5 && (sib >> 6) == 0 && m.mod == 0) bsv = imm32();
      else bsv = (bs == 5 && m.mod != 0) ? cpu.ebp : *reg32(bs);
      base = bsv + (idxv << ss);
      break;
    }
    case 5: base = (m.mod == 0) ? imm32() : cpu.ebp; break;
    case 6: base = cpu.esi; break;
    default: base = cpu.edi; break;
  }
  if (m.mod == 1) base += (int8_t)imm8();
  else if (m.mod == 2) base += imm32();
  m.addr = base;
  return m;
}

/* ------------------------------------------------------------- 指令实现 */
static int g_steps = 0;
static int g_max_steps = 20000000;
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

#define UNIMPL(op)                                                       \
  do {                                                                   \
    fprintf(stderr, "[emu] 未实现指令 %s @ eip=%08x\n", op, cpu.eip - 1); \
    return -1;                                                           \
  } while (0)

static void push32(uint32_t v) { cpu.esp -= 4; wr32(cpu.esp, v); }
static uint32_t pop32(void) { uint32_t v = rd32(cpu.esp); cpu.esp += 4; return v; }

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

/* ---------------------------------------------------------- 导入桩分发 */
static uint32_t arg_at(int i) { return rd32(cpu.esp + 4 + 4 * i); }
static int stub_is(int idx, const char *name) {
  return strcmp(g_stub_name[idx], name) == 0;
}

/* KERNEL32 是 stdcall：被调方负责清参数。桩必须照做，否则栈会每个调用漂 4 字节，
 * 攒到最后 ret 弹出来的就不是返回地址了（真机上表现为跳到假堆地址）。 */
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
    cpu.eax = guest_alloc(arg_at(2) ? arg_at(2) : 64);
  } else if (stub_is(idx, "HeapReAlloc")) {
    /* HeapReAlloc(hHeap, dwFlags, lpMem, dwBytes)：原地返回老指针即可（假堆不回收） */
    cpu.eax = arg_at(2);
  } else if (stub_is(idx, "VirtualAlloc")) {
    /* VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) */
    cpu.eax = guest_alloc(arg_at(1) ? arg_at(1) : 4096);
  } else if (stub_is(idx, "HeapFree") || stub_is(idx, "VirtualFree") ||
             stub_is(idx, "HeapDestroy") || stub_is(idx, "IsBadReadPtr") ||
             stub_is(idx, "IsBadWritePtr")) {
    cpu.eax = stub_is(idx, "HeapFree") || stub_is(idx, "VirtualFree") ? 1 : 0;
  } else if (stub_is(idx, "HeapSize")) {
    cpu.eax = 64;
  } else if (stub_is(idx, "TlsAlloc")) {
    static int n = 0;
    cpu.eax = (uint32_t)(100 + n++);
  } else if (stub_is(idx, "TlsGetValue")) {
    cpu.eax = 0;
  } else if (stub_is(idx, "TlsSetValue") || stub_is(idx, "TlsFree")) {
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
    fprintf(stderr, "[emu] 调用了 %s，停机\n", g_stub_name[idx]);
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
  if (++g_steps > g_max_steps) {
    fprintf(stderr, "[emu] 步数上限 %d 用尽\n", g_max_steps);
    return -1;
  }
  /* 落在导入桩区间：改调宿主实现，然后按 cdecl 返回 */
  if (cpu.eip >= STUB_BASE && cpu.eip < STUB_BASE + STUB_MAX * STUB_STRIDE) {
    int idx = (int)((cpu.eip - STUB_BASE) / STUB_STRIDE);
    if (stub_call(idx) != 0) return -1;
    cpu.eip = pop32();
    cpu.esp += (uint32_t)stub_argc(idx) * 4u;  /* stdcall：被调方清参数 */
    return 0;
  }
  uint32_t start = cpu.eip;
  hist_add(start);
  uint8_t op = imm8();
  int seg_fs = 0;
  int opsize16 = 0;

  /* 段前缀 26/2e/36/3e/64/65、操作数/地址前缀 66/67：忽略（这个 DLL 不用） */
  while (op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0x64 ||
         op == 0x65 || op == 0x66 || op == 0x67 || op == 0xf2 || op == 0xf3) {
    if (op == 0xf3) { /* rep：先只支持 rep stosl/movsl 的最小形态 */
      uint8_t n = imm8();
      if (n == 0xab) { while (cpu.ecx--) { wr32(cpu.edi, cpu.eax); cpu.edi += 4; } return 0; }
      UNIMPL("rep ...");
    }
    if (op == 0x64) seg_fs = 1;   /* fs: —— CRT 用它做 SEH/TLS，给假值即可 */
    if (op == 0x66) opsize16 = 1; /* 操作数 16 位（SYSTEMTIME 那套用 16 位字段） */
    op = imm8();
  }
  (void)start;

  if (op >= 0x50 && op <= 0x57) { push32(*reg32(op - 0x50)); return 0; }
  if (op >= 0x58 && op <= 0x5f) { *reg32(op - 0x58) = pop32(); return 0; }
  if (op >= 0x40 && op <= 0x47) { uint32_t *r = reg32(op - 0x40); alu_add(*r, 1); return 0; }
  if (op >= 0x48 && op <= 0x4f) { uint32_t *r = reg32(op - 0x48); alu_sub(*r, 1); return 0; }
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
    case 0xa0: { uint32_t a = imm32(); *reg8(0) = seg_fs ? 0 : rd8(a); return 0; }
    case 0xa1: { uint32_t a = imm32(); cpu.eax = seg_fs ? 0 : rd32(a); return 0; }
    case 0xa2: { uint32_t a = imm32(); if (!seg_fs) wr8(a, *reg8(0)); return 0; }
    case 0xa3: { uint32_t a = imm32(); if (!seg_fs) wr32(a, cpu.eax); return 0; }
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
      uint32_t b = m.is_reg ? (*reg32(m.addr) & 0xffffu) : (opsize16 ? rd16(m.addr) : rd32(m.addr));
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
    case 0x03: case 0x0b: case 0x23: case 0x2b: case 0x33: {
      ModRM m = modrm();
      uint32_t a = *reg32(m.reg);
      uint32_t b = m.is_reg ? *reg32(m.addr) : rd32(m.addr);
      switch (op) {
        case 0x03: alu_add(a, b); *reg32(m.reg) = a + b; break;
        case 0x0b: *reg32(m.reg) = a | b; set_szp32(a | b); break;
        case 0x23: *reg32(m.reg) = a & b; set_szp32(a & b); break;
        case 0x2b: alu_sub(a, b); *reg32(m.reg) = a - b; break;
        default:   *reg32(m.reg) = a ^ b; set_szp32(a ^ b); break;
      }
      return 0;
    }
    case 0x88: { ModRM m = modrm(); rm_write8(m, *reg8(m.reg)); return 0; }
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
        case 2: { uint32_t t = rm_read32(m); push32(cpu.eip); cpu.eip = t; return 0; } /* call */
        case 4: { uint32_t t = rm_read32(m); cpu.eip = t; return 0; }                 /* jmp */
        case 6: push32(rm_read32(m)); return 0;                                        /* push */
        default: UNIMPL("ff /x");
      }
    }
    case 0x01: case 0x29: case 0x31: case 0x09: case 0x21: case 0x39: case 0x85: {
      ModRM m = modrm();
      uint32_t a = rm_read32(m), b = *reg32(m.reg);
      switch (op) {
        case 0x01: alu_add(a, b); rm_write32(m, a + b); break;
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
    case 0xe8: { int32_t r = (int32_t)imm32(); push32(cpu.eip); cpu.eip += r; return 0; }
    case 0xe9: { int32_t r = (int32_t)imm32(); cpu.eip += r; return 0; }
    case 0xeb: { int8_t r = rel8(); cpu.eip += r; return 0; }
    case 0xc3: cpu.eip = pop32(); return 0;
    case 0xc2: { /* ret imm16：stdcall 的返回并清参数 */
      uint16_t n = rd16(cpu.eip);
      cpu.eip += 2;
      cpu.eip = pop32();
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
    default: UNIMPL("opcode");
  }
}

/* ------------------------------------------------------- PE 装载（最小） */
static int load_pe(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "打不开 %s\n", path); return -1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
  fclose(f);

  uint32_t pe = *(uint32_t *)(buf + 0x3c);
  if (memcmp(buf + pe, "PE\0\0", 4) != 0) { fprintf(stderr, "不是 PE\n"); return -1; }
  uint16_t nsec = *(uint16_t *)(buf + pe + 6);
  uint16_t optsz = *(uint16_t *)(buf + pe + 20);
  uint8_t *opt = buf + pe + 24;
  uint32_t entry = *(uint32_t *)(opt + 16);
  uint8_t *sec = opt + optsz;
  for (int i = 0; i < nsec; ++i, sec += 40) {
    uint32_t vsize = *(uint32_t *)(sec + 8), vaddr = *(uint32_t *)(sec + 12);
    uint32_t rawsz = *(uint32_t *)(sec + 16), rawptr = *(uint32_t *)(sec + 20);
    uint32_t va = IMAGE_BASE + vaddr;
    if (!in_guest(va, vsize ? vsize : rawsz)) { fprintf(stderr, "段越界\n"); return -1; }
    memcpy(gp(va), buf + rawptr, rawsz);
    memset(gp(va) + rawsz, 0, (vsize > rawsz ? vsize - rawsz : 0));
  }
  free(buf);

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

  printf("# 从 %08x 开始执行（先跑 func_load）\n", cpu.eip);
  for (;;) {
    if (step() != 0) break;
    /* 跳到镜像外（且不是导入桩区间）就停下并回溯 */
    const int in_image =
        cpu.eip >= IMAGE_BASE && cpu.eip < IMAGE_BASE + 0x30000u;
    const int in_stub =
        cpu.eip >= STUB_BASE && cpu.eip < STUB_BASE + STUB_MAX * STUB_STRIDE;
    if (!in_image && !in_stub) {
      fprintf(stderr, "[emu] eip=%08x 跑出镜像范围，停止\n", cpu.eip);
      hist_dump();
      break;
    }
  }
  printf("# 停止于 eip=%08x（步数 %d）\n", cpu.eip, g_steps);
  return 0;
}
