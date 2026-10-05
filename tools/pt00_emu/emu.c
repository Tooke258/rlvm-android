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

/* TIB 影子区：`fs:[x]` 一律映射到 TIB_BASE+x。
 * 关键的是 fs:[0] —— 那是 **SEH 异常链的头指针**。之前我们把它当"永远 0"，
 * 于是 CRT 注册的异常处理器被丢掉，一旦 unwind 走到链头就跳到 0
 * （真机表现：eip=00000000）。fs:[0x18] 要存 TIB 自身地址，CRT 会读它。 */
#define TIB_BASE 0x10300000u
static int g_seg_fs = 0;

/* 给导出准备的运行环境（地址都在 guest 空间内，互相不重叠） */
#define SENTINEL 0x30000000u   /* 哨兵返回地址：跑到它就说明"这个函数返回了" */
#define INTD_BASE 0x10400000u  /* intD[2000] */
#define INTF_BASE 0x10500000u  /* intF[2000] */
#define CTX_BASE 0x10600000u   /* 假的引擎上下文；+0x14 处放 intD 基址 */
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
static int g_trace = 0;      /* 1 = 打印每条指令 */
static int g_trace_left = 0; /* 还能打印多少条 */
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
  }
  ++g_frame_n;
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
    if (m.mod != 3 && reg == 0) { float v = *(float *)gp(m.addr); fp_push(v); return 0; }
    if (m.mod != 3 && reg == 2) { *(float *)gp(m.addr) = (float)*XP(0); return 0; }
    if (m.mod != 3 && reg == 3) { *(float *)gp(m.addr) = (float)fp_pop(); return 0; }
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
        case 0xe8: fp_push(1.0); return 0;       /* fld1 */
        case 0xee: fp_push(0.0); return 0;       /* fldz */
        case 0xfa: *XP(0) = sqrt(*XP(0)); return 0;
        case 0xfe: *XP(0) = sin(*XP(0)); return 0;
        case 0xff: *XP(0) = cos(*XP(0)); return 0;
      }
    }
  } else if (op == 0xdb && m.mod != 3) { /* fild m32 / fistp m32 */
    if (reg == 0) { fp_push((double)*(int32_t *)gp(m.addr)); return 0; }
    if (reg == 3) { uint32_t v = (uint32_t)fp_to_int(*XP(0)); fp_pop(); wr32(m.addr, v); return 0; }
  } else if (op == 0xdc) { /* m64real */
    if (m.mod != 3) {
      double v = *(double *)gp(m.addr);
      switch (reg) {
        case 0: *XP(0) += v; return 0;
        case 1: *XP(0) *= v; return 0;
        case 4: *XP(0) -= v; return 0;
        case 5: *XP(0) = v - *XP(0); return 0;
        case 6: *XP(0) /= v; return 0;
        case 7: *XP(0) = v / *XP(0); return 0;
      }
    } else {
      switch (reg) {
        case 0: *XP(m.rm) += *XP(0); return 0;
        case 1: *XP(m.rm) *= *XP(0); return 0;
        case 4: *XP(m.rm) -= *XP(0); return 0;
        case 5: { double t = *XP(0) - *XP(m.rm); *XP(m.rm) = t; return 0; }
        case 6: *XP(m.rm) /= *XP(0); return 0;
        case 7: { double t = *XP(0) / *XP(m.rm); *XP(m.rm) = t; return 0; }
      }
    }
  } else if (op == 0xdd && m.mod != 3) { /* fld/fst/fstp m64 */
    if (reg == 0) { fp_push(*(double *)gp(m.addr)); return 0; }
    if (reg == 2) { *(double *)gp(m.addr) = *XP(0); return 0; }
    if (reg == 3) { *(double *)gp(m.addr) = fp_pop(); return 0; }
  } else if (op == 0xde) { /* 出栈式算术 + fcompp */
    if (m.mod != 3 && reg == 0) { fp_pop(); return 0; } /* fiadd m16：少见，先按空过 */
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
        case 4: *XP(1) -= *XP(0); fp_pop(); return 0;  /* fsubp */
        case 5: { double t = *XP(0) - *XP(1); fp_pop(); *XP(0) = t; return 0; } /* fsubrp */
        case 6: *XP(1) /= *XP(0); fp_pop(); return 0;  /* fdivp */
        case 7: { double t = *XP(0) / *XP(1); fp_pop(); *XP(0) = t; return 0; } /* fdivrp */
      }
    }
  } else if (op == 0xdf) {
    if (m.mod == 3 && m.raw == 0xe0) { /* fnstsw ax */
      cpu.eax = (cpu.eax & 0xffff0000u) | (uint32_t)g_fpu_sw;
      return 0;
    }
    if (m.mod != 3 && reg == 5) { fp_push((double)*(int16_t *)gp(m.addr)); return 0; } /* fild m16 */
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
    uint32_t ret = rd32(cpu.esp);
    if (emu_ret_check() != 0) return -1;
    cpu.eip = ret;
    cpu.esp += (uint32_t)stub_argc(idx) * 4u;  /* stdcall：被调方清参数 */
    return 0;
  }
  if (g_trace && g_trace_left-- > 0) {
    fprintf(stderr,
            "  %08x eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x"
            " st0=%.6g st1=%.6g st2=%.6g sw=%04x\n",
            cpu.eip, cpu.eax, cpu.ecx, cpu.edx, cpu.esp, cpu.ebp, *XP(0), *XP(1),
            *XP(2), g_fpu_sw);
  }
  uint32_t start = cpu.eip;
  hist_add(start);
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
                  uint32_t q = cpu.eax / v, r2 = cpu.eax % v; cpu.eax = q; cpu.edx = r2; return 0; }
        case 7: { if (v == 0) { fprintf(stderr, "[emu] idiv 0\n"); return -1; }
                  int32_t q = (int32_t)cpu.eax / (int32_t)v, r2 = (int32_t)cpu.eax % (int32_t)v;
                  cpu.eax = (uint32_t)q; cpu.edx = (uint32_t)r2; return 0; }
      }
      UNIMPL("f7 /x");
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

  /* TIB 影子：fs:[0x18] 存 TIB 自身地址；fs:[0]（SEH 链头）初始为 -1（无处理器） */
  wr32(TIB_BASE + 0x18, TIB_BASE);
  wr32(TIB_BASE + 0x00, 0xffffffffu);
  /* 引擎上下文：+0x14 处放 intD 基址（这是 PT00 唯一用到的约定） */
  wr32(CTX_BASE + 0x14, INTD_BASE);

  /* 跑一个导出：入口处压哨兵当返回地址，跑到哨兵即视为正常返回。
   * 参数按 stdcall 约定从右往左压，最上面是哨兵。 */
  #define RUN_EXPORT(entry, argc, ...)                              \
    do {                                                            \
      uint32_t a_[8] = {0, 0, 0, 0, 0, 0, 0, 0};                    \
      const uint32_t vals_[8] = {__VA_ARGS__};                      \
      for (int i_ = 0; i_ < (argc) && i_ < 8; ++i_) a_[i_] = vals_[i_]; \
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
      if (rc_ == 0) { cpu.esp += (argc) * 4u; } /* stdcall：清参数 */  \
      g_last_rc = rc_;                                              \
    } while (0)

  printf("# 跑 func_load(%08x)\n", CTX_BASE);
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

  printf("# 跑 func_init\n");
  RUN_EXPORT(IMAGE_BASE + 0x1670, 0);
  printf("# func_init 结束 rc=%d\n", g_last_rc);

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
        if (strcmp(argv[i], "--trace-func") == 0 && i + 1 < argc &&
            v[0] == atoi(argv[i + 1])) {
          g_trace = 1;
          g_trace_left = 3000;
          fprintf(stderr, "# trace func=%d\n", v[0]);
          break;
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
  printf("# 结束于 eip=%08x（步数 %d）\n", cpu.eip, g_steps);
  return 0;
}
