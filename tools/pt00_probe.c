/* PT00 锚点探针：**只读**扫描进程内存，反推 RLVM 的 `intD` 基址，
 * 再把 A2 锚点要的那一族整数打出来（格式对齐 Android 侧 `[pt00] frame` 快照）。
 *
 * 为什么先搜「签名窗口」而不是先搜「值 10」：单个值 10 在 100+ MB 里能命中上千处，
 * 取前几个几乎必然是垃圾。脚本可见状态里有一串**连着的**值（真机快照）：
 *     intD[70..76] = 20, 19, 15, 10, 0, -1, 1
 * 连着的 7 个值几乎唯一，命中即为 intD 基址。单值模式则用这 7 项（去掉 73 自己）
 * 给候选打分排序，仍然只打印得分最高的几条。
 *
 * 用法（**输出一律 ASCII**：cmd 用 CP936 解码 UTF-8 中文会变乱码，所以运行期消息不用中文）:
 *   pt00_probe.exe --scan-all            在所有进程里找（不用知道进程名，推荐）
 *   pt00_probe.exe --list [substr]       列出进程（pid / 内存 / 名字），可带过滤串
 *   pt00_probe.exe <进程名|PID>          签名窗口模式
 *   pt00_probe.exe <进程名|PID> <值> [条数]   单值模式：把 <值> 当作 intD[73]
 *   pt00_probe.exe --selftest            自检：在本进程里放一份合成 intD 再扫自己
 * 构建: tools\build_pt00_probe.bat
 * 安全性: 只请求 QUERY_LIMITED_INFORMATION / QUERY_INFORMATION / VM_READ —— 只读，绝不写目标进程。 */
#define _CRT_SECURE_NO_WARNINGS /* strncpy 在 MSVC 里报 C4996；这里用法是有界的 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDX_ANCHOR 73
#define MAX_CANDIDATES 20000

/* 默认搜索的签名窗口（intD[70..76]，来自 Android 真机快照）。 */
#define SIG_IDX0 70
static const int kSig[] = {20, 19, 15, 10, 0, -1, 1};
static const int kSigN = (int)(sizeof(kSig) / sizeof(kSig[0]));

/* 给单值模式的候选打分用的邻居（不含 73 自己）。 */
typedef struct { int idx; int want; } Expect;
static const Expect kNeighbor[] = {
  {70, 20}, {71, 19}, {72, 15}, {74, 0}, {75, -1}, {76, 1},
};
static const int kNeighborN = (int)(sizeof(kNeighbor) / sizeof(kNeighbor[0]));

#define SANE_NDX 2000        /* 「像脚本变量」统计 / --full 的整片范围：intD[0..1999] */
#define SANE_LIMIT 1000000   /* |v| <= 1e6 视为「像脚本变量」 */

/* 命中后要打印的下标。第一行是 A2 锚点表里的老字段，
 * 后面几段是这轮按 SEEN7110/7420/7450 的脚本语义补的：
 *   40 = 121（相位标志之一）· 70..99（相位族 + 打者开关 80 / 球场标志 79,82,83,85,86,87,88）
 *   300..303（打者源矩形）· 510/520..529（传球/位置镜像）· 600..645（球/打者状态）
 *   700..790（回合标志）· 900..975（球网格 intD[900+i*7+j]） */
static const int kInts[] = {30, 46, 210, 320, 350, 700, 710, 734, 740, 771,
  1800, 1801, 1802, 1803, 1804,
  1830, 1831, 1832, 1833, 1834, 1835, 1836, 1837, 1838, 1839, 1840, 1841,
  1850, 1851, 1852, 1853, 1854, 1855, 1856, 1857, 1858, 1859, 1860, 1861, 1862, 1863, 1864, 1865,
  40, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
  95, 96, 97, 98, 99,
  300, 301, 302, 303, 304, 305, 306, 307,
  510, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529,
  600, 601, 602, 603, 604, 605, 610, 611, 612, 613, 620, 621, 622, 623, 624, 625, 630, 635,
  717, 718, 719, 720, 730, 731, 732, 733, 741, 742, 743, 744, 750, 751, 752, 760, 761, 762,
  770, 772, 773, 774, 775, 776, 777, 778, 779, 780, 790};

/* --full：把 intD[0..1999] 全打出来（一行 16 个），便于两侧整片 diff。 */
static void DumpFull(HANDLE h, uint64_t base) {
  static int buf[SANE_NDX];
  SIZE_T got = 0;
  if (!ReadProcessMemory(h, (LPCVOID)(uintptr_t)base, buf, sizeof(buf), &got) || got < 4) {
    printf("  <intD[0..%d] unreadable>\n", SANE_NDX - 1);
    return;
  }
  int n = (int)(got / 4);
  printf("  full intD[0..%d] (%d values), 16 per line:\n", n - 1, n);
  for (int i = 0; i < n; ++i) {
    if (i % 16 == 0) printf("    [%4d]", i);
    printf(" %d", buf[i]);
    if (i % 16 == 15 || i == n - 1) printf("\n");
  }
}

/* base = intD 基址；score = 邻居签名命中数；sane = intD[0..1999] 里「像脚本变量」的个数 */
typedef struct { uint64_t base; int score; int sane; } Cand;

/* 扫描时要跳过的地址区间（自检用：免得命中探针自己 .rdata 里的签名常量） */
static uint64_t g_excl_lo = 0, g_excl_hi = 0;

/* --scan-all 只扫「有可能装着引擎」的进程，别去啃一堆 3 MB 的系统服务 */
#define SCANALL_MIN_WS (32u << 20)

static int g_full = 0; /* --full：候选块之后再整片打印 intD[0..1999] */

typedef struct { DWORD pid; char name[64]; SIZE_T ws; int readable; } ProcInfo;

static int ReadInt(HANDLE h, uint64_t a, int *out) {
  SIZE_T got = 0;
  return ReadProcessMemory(h, (LPCVOID)(uintptr_t)a, out, sizeof(int), &got) && got == sizeof(int);
}

static HANDLE OpenForScan(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  return h;
}

/* 枚举进程：名字 + 工作集 + 能不能读内存。
 * 注意: QueryFullProcessImageNameA 的 pdwSize 是**入参**（缓冲区字符数），必须先填 MAX_PATH；
 * 填 0 会直接失败（这个坑踩过一次，症状是「按名字永远找不到进程」）。 */
static int EnumProcs(ProcInfo *out, int cap) {
  DWORD need = 0;
  const DWORD kCap = 4096;
  DWORD *pids = (DWORD *)malloc(sizeof(DWORD) * kCap);
  if (!pids) return 0;
  if (!EnumProcesses(pids, sizeof(DWORD) * kCap, &need)) { free(pids); return 0; }
  DWORD n = need / sizeof(DWORD);
  if (n > kCap) n = kCap;
  int cnt = 0;
  for (DWORD i = 0; i < n && cnt < cap; ++i) {
    if (!pids[i] || pids[i] == GetCurrentProcessId()) continue;
    HANDLE h = OpenForScan(pids[i]);
    if (!h) continue;
    ProcInfo pi; memset(&pi, 0, sizeof(pi));
    pi.pid = pids[i];
    pi.readable = 1;
    char path[MAX_PATH] = {0}; DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameA(h, 0, path, &len)) {
      const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
      strncpy(pi.name, b, 63);
    } else {
      strncpy(pi.name, "?", 63);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) pi.ws = pmc.WorkingSetSize;
    CloseHandle(h);
    out[cnt++] = pi;
  }
  free(pids);
  return cnt;
}

static int CmpProcByWs(const void *a, const void *b) {
  const ProcInfo *x = (const ProcInfo *)a, *y = (const ProcInfo *)b;
  if (x->ws != y->ws) return (y->ws > x->ws) ? 1 : -1; /* 内存大的在前 */
  return (x->pid < y->pid) ? -1 : 1;
}

static int ScoreCandidate(HANDLE h, uint64_t base) {
  int s = 0;
  for (int i = 0; i < kNeighborN; ++i) {
    int v = 0;
    if (ReadInt(h, base + (uint64_t)kNeighbor[i].idx * 4, &v) && v == kNeighbor[i].want) ++s;
  }
  return s;
}

/* 「像不像一块脚本变量数组」：整数数组里绝大多数条目应该是有界的小值/0；
 * 代码段或指针表则大片是 0x7Fxxxxxx 这种大数。这条能把噪音候选一眼挑掉。 */
static int SaneCount(HANDLE h, uint64_t base) {
  static int buf[SANE_NDX];
  SIZE_T got = 0;
  if (!ReadProcessMemory(h, (LPCVOID)(uintptr_t)base, buf, sizeof(buf), &got) || got < 4)
    return -1;
  int n = (int)(got / 4), s = 0;
  for (int i = 0; i < n; ++i) if (buf[i] <= SANE_LIMIT && buf[i] >= -SANE_LIMIT) ++s;
  return s;
}

/* base = intD 基址（下标*4 即偏移）；反推 intD[73] 的地址只是为了打印。 */
static void DumpCandidate(HANDLE h, uint64_t base, int score, int sane, int no) {
  uint64_t anchor = base + (uint64_t)IDX_ANCHOR * 4;
  printf("\n--- candidate #%d: intD[%d] @ 0x%llx => intD base = 0x%llx   neighbors %d/%d  sane %d/%d%s ---\n",
         no, IDX_ANCHOR, (unsigned long long)anchor, (unsigned long long)base,
         score, kNeighborN, sane, SANE_NDX,
         (score == kNeighborN && sane > SANE_NDX * 9 / 10) ? "  [MOST LIKELY]" : "");
  for (size_t i = 0; i < sizeof(kInts) / sizeof(kInts[0]); ++i) {
    int v = 0;
    if (ReadInt(h, base + (uint64_t)kInts[i] * 4, &v)) printf("intD[%d]=%d", kInts[i], v);
    else printf("intD[%d]=<unreadable>", kInts[i]);
    fputs(((i + 1) % 8) ? "  " : "\n", stdout);
  }
  printf("\n  phase 70..95 =");
  for (int i = 70; i <= 95; ++i) {
    int v = 0;
    printf(ReadInt(h, base + (uint64_t)i * 4, &v) ? " %d" : " ?", v);
  }
  printf("\n");
  if (g_full) DumpFull(h, base);
}

static int CmpCand(const void *a, const void *b) {
  const Cand *x = (const Cand *)a, *y = (const Cand *)b;
  if (x->sane != y->sane) return y->sane - x->sane;        /* 先看「像脚本变量」的比例 */
  if (x->score != y->score) return y->score - x->score;    /* 再看邻居签名 */
  return (x->base < y->base) ? -1 : (x->base > y->base);   /* 最后按地址升序 */
}

/* 扫描 h 的地址空间，打印前 maxc 条候选；*best_base 回填榜首的基址（0 = 没找到）。
 * quiet=1 时不打印表头/候选块，只回结果（--scan-all 用来先探路）。 */
static int ScanAndDump(HANDLE h, DWORD pid, int have_anchor, int anchor, int maxc,
                       uint64_t *best_base, int quiet) {
  SYSTEM_INFO si = {0}; GetSystemInfo(&si);
  uint64_t addr = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress;
  uint64_t end = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;
  const SIZE_T kChunk = 4u << 20;
  unsigned char *buf = (unsigned char *)malloc(kChunk);
  Cand *cands = (Cand *)malloc(sizeof(Cand) * MAX_CANDIDATES);
  if (!buf || !cands) { fprintf(stderr, "out of memory\n"); free(buf); free(cands); return 2; }

  if (!quiet) {
    printf("# pid=%lu mode=%s score-signature=%d\n", (unsigned long)pid,
           have_anchor ? "single value (treat as intD[73])" : "signature window (intD[70..76])",
           kNeighborN);
    if (!have_anchor) {
      printf("# signature =");
      for (int i = 0; i < kSigN; ++i) printf(" %d", kSig[i]);
      printf("  (intD[%d..%d])\n", SIG_IDX0, SIG_IDX0 + kSigN - 1);
    }
  }
  int n = 0, truncated = 0;
  uint64_t scanned = 0;
  while (addr < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQueryEx(h, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    uint64_t reg = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = (uint64_t)mbi.RegionSize;
    if (sz == 0) break;
    int readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                   (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (readable) {
      for (uint64_t off = 0; off + 4 <= sz; off += kChunk) {
        SIZE_T want = (SIZE_T)((sz - off) > kChunk ? kChunk : (sz - off)), got = 0;
        if (!ReadProcessMemory(h, (LPCVOID)(uintptr_t)(reg + off), buf, want, &got) || got < 4) {
          continue; /* 读不了的区域跳过；off 仍按 kChunk 前进，不会死循环 */
        }
        scanned += got;
        if (!have_anchor) {
          /* 签名窗口要求整段落在本次读入的缓冲里；跨 4MB 块边界最多漏 6 个 int，可接受。 */
          for (SIZE_T i = 0; i + 4 * (SIZE_T)kSigN <= got; i += 4) {
            int v = 0; memcpy(&v, buf + i, 4);
            if (v != kSig[0]) continue;
            int match = 1;
            for (int k = 1; k < kSigN; ++k) {
              int w = 0; memcpy(&w, buf + i + 4 * k, 4);
              if (w != kSig[k]) { match = 0; break; }
            }
            if (!match) continue;
            uint64_t hit = reg + off + i;
            if (g_excl_lo && hit >= g_excl_lo && hit < g_excl_hi) continue;
            if (n >= MAX_CANDIDATES) { truncated = 1; break; }
            cands[n].base = hit - (uint64_t)SIG_IDX0 * 4; /* = intD 基址 */
            cands[n].score = -1;
            cands[n].sane = -1;
            ++n;
          }
        } else {
          for (SIZE_T i = 0; i + 4 <= got; i += 4) {
            int v = 0; memcpy(&v, buf + i, 4);
            if (v != anchor) continue;
            uint64_t hit = reg + off + i;
            if (g_excl_lo && hit >= g_excl_lo && hit < g_excl_hi) continue;
            if (n >= MAX_CANDIDATES) { truncated = 1; break; }
            cands[n].base = hit - (uint64_t)IDX_ANCHOR * 4; /* = intD 基址 */
            cands[n].score = -1;
            cands[n].sane = -1;
            ++n;
          }
        }
        if (truncated) break;
      }
    }
    addr = reg + sz;
    if (truncated) break;
  }

  for (int i = 0; i < n; ++i) {
    cands[i].score = ScoreCandidate(h, cands[i].base);
    cands[i].sane = SaneCount(h, cands[i].base);
  }
  qsort(cands, (size_t)n, sizeof(Cand), CmpCand);

  *best_base = n ? cands[0].base : 0;
  if (!quiet) {
    printf("# scanned %.1f MB, candidates %d%s\n", scanned / 1048576.0, n,
           truncated ? " (truncated at 20000)" : "");
  }
  int rc = n ? 0 : 1;
  if (!n) {
    if (quiet) {
      printf("  scanned %.1f MB -> no hit\n", scanned / 1048576.0);
    } else {
      printf("not found: make sure the game is sitting on the **minigame waiting screen**\n");
      printf("(at that moment intD[70..76] = 20,19,15,10,0,-1,1).\n");
      printf("if you are sure it is, retry with the single-value mode: pt00_probe.exe <proc> 10\n");
    }
  } else {
    int top = (n < maxc) ? n : maxc;
    if (!quiet) {
      for (int i = 0; i < top; ++i)
        DumpCandidate(h, cands[i].base, cands[i].score, cands[i].sane, i + 1);
      printf("\n# paste the intD[...] block of the [MOST LIKELY] candidate back for the A2 compare.\n");
    }
  }
  free(buf); free(cands);
  return rc;
}

/* 在所有（够大的）进程里找签名——不用知道进程名，用来对付「汉化启动器 + 未知引擎进程名」。 */
static int ScanAll(int have_anchor, int anchor, int maxc) {
  ProcInfo *procs = (ProcInfo *)malloc(sizeof(ProcInfo) * 4096);
  if (!procs) return 2;
  int n = EnumProcs(procs, 4096);
  qsort(procs, (size_t)n, sizeof(ProcInfo), CmpProcByWs);
  printf("# --scan-all: %d readable processes, scanning the ones with working set >= %u MB\n",
         n, (unsigned)(SCANALL_MIN_WS >> 20));
  int hits = 0, scanned = 0;
  for (int i = 0; i < n; ++i) {
    if (procs[i].ws < SCANALL_MIN_WS) continue;
    ++scanned;
    printf("\n=== [%d/%d] pid=%lu  ws=%llu MB  %s ===\n", i + 1, n, (unsigned long)procs[i].pid,
           (unsigned long long)(procs[i].ws >> 20), procs[i].name);
    fflush(stdout);
    HANDLE h = OpenForScan(procs[i].pid);
    if (!h) { printf("  (open failed)\n"); continue; }
    uint64_t best = 0;
    int hit = (ScanAndDump(h, procs[i].pid, have_anchor, anchor, maxc, &best, 1) == 0);
    if (hit) {
      ++hits;
      printf("# ^^^ HIT in pid=%lu (%s) = %s\n", (unsigned long)procs[i].pid, procs[i].name,
             have_anchor ? "single-value anchor" : "signature window");
      /* 命中后再按正常格式打印候选块 */
      ScanAndDump(h, procs[i].pid, have_anchor, anchor, maxc, &best, 0);
      CloseHandle(h);
      break; /* 找到就收工：同一时刻只有一个进程是真的引擎 */
    }
    CloseHandle(h);
  }
  if (!hits) printf("\n# no hit in %d scanned processes. (Game not running? Not on the waiting screen? Launcher elevated?)\n", scanned);
  free(procs);
  return hits ? 0 : 1;
}

/* 在目标进程里找一段 ASCII 文本（用来回答「哪张对象贴图真的被加载了」——
 * 引擎把文件名存成字符串，Android 侧我们靠图形栈转储看同一个问题）。 */
static int FindStr(DWORD pid, const char *needle, int max_hits) {
  size_t nlen = strlen(needle);
  if (nlen == 0) { fprintf(stderr, "empty pattern\n"); return 2; }
  HANDLE h = OpenForScan(pid);
  if (!h) {
    fprintf(stderr, "OpenProcess failed pid=%lu err=%lu\n", (unsigned long)pid,
            (unsigned long)GetLastError());
    return 1;
  }
  SYSTEM_INFO si = {0}; GetSystemInfo(&si);
  uint64_t addr = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress;
  uint64_t end = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;
  const SIZE_T kChunk = 4u << 20;
  unsigned char *buf = (unsigned char *)malloc(kChunk + 64);
  if (!buf) { CloseHandle(h); return 2; }
  printf("# find \"%s\" in pid=%lu (up to %d hits)\n", needle, (unsigned long)pid, max_hits);
  int hits = 0;
  while (addr < end && hits < max_hits) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQueryEx(h, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    uint64_t reg = (uint64_t)(uintptr_t)mbi.BaseAddress, sz = (uint64_t)mbi.RegionSize;
    if (sz == 0) break;
    int readable = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                   (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    if (readable) {
      for (uint64_t off = 0; off < sz && hits < max_hits; off += kChunk) {
        SIZE_T want = (SIZE_T)((sz - off) > kChunk ? kChunk : (sz - off)), got = 0;
        if (!ReadProcessMemory(h, (LPCVOID)(uintptr_t)(reg + off), buf, want, &got) || got < nlen)
          continue;
        for (SIZE_T i = 0; i + nlen <= got && hits < max_hits; ++i) {
          if (buf[i] != (unsigned char)needle[0]) continue;
          if (memcmp(buf + i, needle, nlen) != 0) continue;
          /* 打印命中处前后一小段可打印字符，便于判断是不是文件名 */
          SIZE_T s = (i >= 24) ? i - 24 : 0;
          printf("  hit @ 0x%llx : \"", (unsigned long long)(reg + off + i));
          for (SIZE_T k = s; k < got && k < i + nlen + 24; ++k) {
            unsigned char c = buf[k];
            putchar((c >= 32 && c < 127) ? (char)c : '.');
          }
          printf("\"\n");
          {
            /* 反查「宿主引擎里的图形对象结构」：对象里存着资源名，附近就是 x/y/可见性。
             * 这里把命中处前后各 64 字节按 hex + int32 打出来。 */
            SIZE_T b = (i >= 64) ? i - 64 : 0;
            SIZE_T e = (i + nlen + 64 < got) ? (i + nlen + 64) : got;
            printf("    hex:");
            for (SIZE_T k = b; k < e; ++k) {
              if ((k - b) % 16 == 0) printf("\n      %+04d:", (int)(k - i));
              printf(" %02x", buf[k]);
            }
            printf("\n    i32:");
            for (SIZE_T k = b; k + 4 <= e; k += 4) {
              int32_t v;
              memcpy(&v, buf + k, 4);
              printf(" [%+d]=%d", (int)(k - i), (int)v);
            }
            printf("\n");
          }
          ++hits;
        }
      }
    }
    addr = reg + sz;
  }
  printf("# %d hit(s)\n", hits);
  free(buf); CloseHandle(h);
  return hits ? 0 : 1;
}

static int ListProcs(const char *filter) {
  ProcInfo *procs = (ProcInfo *)malloc(sizeof(ProcInfo) * 4096);
  if (!procs) return 2;
  int n = EnumProcs(procs, 4096);
  qsort(procs, (size_t)n, sizeof(ProcInfo), CmpProcByWs);
  printf("# %d readable processes%s%s\n", n, filter ? " matching " : "", filter ? filter : "");
  int shown = 0;
  for (int i = 0; i < n; ++i) {
    if (filter && *filter) {
      char lower[64], f[64];
      strncpy(lower, procs[i].name, 63); lower[63] = 0;
      strncpy(f, filter, 63); f[63] = 0;
      _strlwr(lower); _strlwr(f);
      if (!strstr(lower, f)) continue;
    }
    printf("  pid=%-6lu ws=%6llu MB  %s\n", (unsigned long)procs[i].pid,
           (unsigned long long)(procs[i].ws >> 20), procs[i].name);
    ++shown;
  }
  printf("# %d shown. usage: pt00_probe.exe <name or pid>   (or simply: pt00_probe.exe --scan-all)\n",
         shown);
  free(procs);
  return 0;
}

/* 自检：在本进程里摆一份合成的 intD，再拿探针去扫自己，验证「找到 + 基址正确」。 */
static int SelfTest(void) {
  static int fake_intd[4096];
  for (int i = 0; i < 4096; ++i) fake_intd[i] = 100000 + i; /* 先填成别的，避免假命中 */
  for (int i = 0; i < kSigN; ++i) fake_intd[SIG_IDX0 + i] = kSig[i];
  uint64_t expect = (uint64_t)(uintptr_t)&fake_intd[0];
  printf("# selftest: fake intD base = 0x%llx\n", (unsigned long long)expect);
  /* 探针自己的 .rdata 里就存着签名常量，扫自己时必然命中它 —— 明确排除掉。 */
  g_excl_lo = (uint64_t)(uintptr_t)&kSig[0];
  g_excl_hi = g_excl_lo + sizeof(kSig) + 64;
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
  if (!h) {
    fprintf(stderr, "selftest: OpenProcess(self) failed err=%lu\n", (unsigned long)GetLastError());
    return 2;
  }
  uint64_t got = 0;
  int rc = ScanAndDump(h, GetCurrentProcessId(), 0, 0, 3, &got, 0);
  CloseHandle(h);
  int ok = (rc == 0 && got == expect);
  printf("\n# selftest: %s (expected 0x%llx, got 0x%llx)\n", ok ? "PASS" : "FAIL",
         (unsigned long long)expect, (unsigned long long)got);
  return ok ? 0 : 1;
}

/* "名字或 PID" -> PID；失败时把「枚举到几个 / 能开几个 / 像名字的有哪些」打出来。 */
static DWORD ResolvePid(const char *spec) {
  if (spec[0] >= '0' && spec[0] <= '9') {
    DWORD pid = (DWORD)strtoul(spec, NULL, 10);
    if (pid) return pid;
  }
  ProcInfo *procs = (ProcInfo *)malloc(sizeof(ProcInfo) * 4096);
  if (!procs) return 0;
  int n = EnumProcs(procs, 4096);
  char q[64];
  strncpy(q, spec, 63); q[63] = 0; _strlwr(q);
  DWORD pid = 0;
  for (int i = 0; i < n && !pid; ++i) {
    char lower[64];
    strncpy(lower, procs[i].name, 63); lower[63] = 0; _strlwr(lower);
    if (strcmp(lower, q) == 0 || (strlen(q) >= 3 && strstr(lower, q))) pid = procs[i].pid;
  }
  if (!pid) {
    fprintf(stderr, "process \"%s\" not found among %d readable processes.\n", spec, n);
    fprintf(stderr, "  candidates containing \"%s\":\n", spec);
    int shown = 0;
    for (int i = 0; i < n && shown < 20; ++i) {
      char ln[64];
      strncpy(ln, procs[i].name, 63); ln[63] = 0; _strlwr(ln);
      if (strstr(ln, q)) { fprintf(stderr, "    pid=%lu  %s\n", (unsigned long)procs[i].pid, procs[i].name); ++shown; }
    }
    if (!shown) {
      fprintf(stderr, "    (none) -- try: pt00_probe.exe --scan-all\n");
      fprintf(stderr, "    or list everything: pt00_probe.exe --list\n");
    }
    free(procs);
    return 0;
  }
  free(procs);
  return pid;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: pt00_probe.exe --scan-all [anchor_value]\n"
      "       pt00_probe.exe --list [substr]\n"
      "       pt00_probe.exe --find-str <process name|pid> <text> [max_hits=20]\n"
      "       pt00_probe.exe <process name|pid> [--full] [anchor_value] [max_candidates=4]\n"
      "       pt00_probe.exe --selftest\n"
      "  no anchor_value -> search the signature window intD[70..76] = 20,19,15,10,0,-1,1\n"
      "  with anchor_value -> treat it as intD[73], scan by value and score by the neighbors\n"
      "  --full -> also dump the whole intD[0..1999] (for a side-by-side diff)\n");
    return 2;
  }
  if (strcmp(argv[1], "--selftest") == 0) return SelfTest();
  if (strcmp(argv[1], "--list") == 0) return ListProcs((argc > 2) ? argv[2] : NULL);
  if (strcmp(argv[1], "--find-str") == 0) {
    if (argc < 4) { fprintf(stderr, "usage: pt00_probe.exe --find-str <proc> <text> [max_hits]\n"); return 2; }
    DWORD fpid = ResolvePid(argv[2]);
    if (!fpid) return 1;
    return FindStr(fpid, argv[3], (argc > 4) ? atoi(argv[4]) : 20);
  }
  /* --full 可以出现在任意位置：任何参数里出现它，就整片打印 intD[0..1999] */
  for (int i = 2; i < argc; ++i) if (strcmp(argv[i], "--full") == 0) g_full = 1;
  if (strcmp(argv[1], "--scan-all") == 0) {
    int have_anchor = (argc > 2 && strcmp(argv[2], "--full") != 0);
    return ScanAll(have_anchor, have_anchor ? atoi(argv[2]) : 0, 4);
  }

  DWORD pid = ResolvePid(argv[1]);
  if (!pid) return 1;

  int have_anchor = (argc > 2 && strcmp(argv[2], "--full") != 0);
  int anchor = have_anchor ? atoi(argv[2]) : 0;
  int maxc = (argc > 3 && strcmp(argv[3], "--full") != 0) ? atoi(argv[3]) : 4;
  if (maxc < 1) maxc = 1;

  HANDLE h = OpenForScan(pid);
  if (!h) {
    fprintf(stderr, "OpenProcess failed pid=%lu err=%lu (try running as administrator)\n",
            (unsigned long)pid, (unsigned long)GetLastError());
    return 1;
  }
  uint64_t best = 0;
  int rc = ScanAndDump(h, pid, have_anchor, anchor, maxc, &best, 0);
  CloseHandle(h);
  return rc;
}
