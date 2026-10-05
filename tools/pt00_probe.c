/* PT00 锚点探针：**只读**扫描另一个进程的内存，反推 RLVM 的 `intD` 基址，
 * 再把 A2 锚点要的那一族整数打出来（格式对齐 Android 侧 `[pt00] frame` 快照）。
 *
 * 为什么先搜「签名窗口」而不是先搜「值 10」：单个值 10 在 100+ MB 里能命中上千处，
 * 取前几个几乎必然是垃圾。脚本可见状态里有一串**连着的**值（真机快照）：
 *     intD[70..76] = 20, 19, 15, 10, 0, -1, 1
 * 连着的 7 个值几乎唯一，命中即为 intD 基址。单值模式则用这 7 项（去掉 73 自己）
 * 给候选打分排序，仍然只打印得分最高的几条。
 *
 * 用法:
 *   pt00_probe.exe <进程名|PID>              签名窗口模式（推荐）
 *   pt00_probe.exe <进程名|PID> <值> [条数]  单值模式：把 <值> 当作 intD[73]
 *   pt00_probe.exe --selftest               自检：在本进程里放一份合成 intD 再扫自己
 * 构建: tools\build_pt00_probe.bat
 * 安全性: 只请求 PROCESS_QUERY_LIMITED_INFORMATION / PROCESS_QUERY_INFORMATION /
 *         PROCESS_VM_READ —— 只读，绝不写目标进程内存。 */
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

/* 命中后整段打印的下标（A2 锚点表）。 */
static const int kInts[] = {30, 46, 210, 320, 350, 700, 710, 734, 740, 771,
  1800, 1801, 1802, 1803, 1804,
  1830, 1831, 1832, 1833, 1834, 1835, 1836, 1837, 1838, 1839, 1840, 1841,
  1850, 1851, 1852, 1853, 1854, 1855, 1856, 1857, 1858, 1859, 1860, 1861, 1862, 1863, 1864, 1865,
  70, 71, 72, 73, 74, 75, 76, 79, 90};

/* base = intD 基址；score = 邻居签名命中数；sane = intD[0..1999] 里「像脚本变量」的个数 */
typedef struct { uint64_t base; int score; int sane; } Cand;

/* 扫描时要跳过的地址区间（自检用：免得命中探针自己 .rdata 里的签名常量） */
static uint64_t g_excl_lo = 0, g_excl_hi = 0;

#define SANE_NDX 2000        /* 统计范围：intD[0..1999] */
#define SANE_LIMIT 1000000   /* |v| <= 1e6 视为「像脚本变量」 */

static int ReadInt(HANDLE h, uint64_t a, int *out) {
  SIZE_T got = 0;
  return ReadProcessMemory(h, (LPCVOID)(uintptr_t)a, out, sizeof(int), &got) && got == sizeof(int);
}

/* 名字 -> PID。同时回报「枚举到多少个进程 / 多少个能打开」，
 * 这样失败时能立刻区分「名字写错」和「权限不够」（沙箱账户看不到用户进程）。 */
static DWORD FindPidByName(const char *name, int *enumerated, int *opened,
                          char seen[][64], int seen_cap, int *seen_n) {
  DWORD cap = 4096, need = 0;
  DWORD *pids = (DWORD *)malloc(sizeof(DWORD) * cap);
  if (!pids) return 0;
  if (!EnumProcesses(pids, sizeof(DWORD) * cap, &need)) { free(pids); return 0; }
  DWORD n = need / sizeof(DWORD);
  if (n > cap) n = cap;
  DWORD found = 0;
  for (DWORD i = 0; i < n && !found; ++i) {
    if (!pids[i]) continue;
    ++*enumerated;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
    if (!h) continue;
    ++*opened;
    /* 注意: QueryFullProcessImageNameA 的 pdwSize 是**入参**（缓冲区字符数），
     * 必须先填 MAX_PATH；填 0 会直接失败（这个坑踩过一次）。 */
    char path[MAX_PATH] = {0}; DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameA(h, 0, path, &len)) {
      const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
      if (*seen_n < seen_cap) {
        strncpy(seen[*seen_n], b, 63); seen[*seen_n][63] = 0; ++*seen_n;
      }
      if (_stricmp(b, name) == 0 || (strlen(name) >= 4 && strstr(b, name) != NULL))
        found = pids[i];
    }
    CloseHandle(h);
  }
  free(pids);
  return found;
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
  printf("\n--- 候选 #%d: intD[%d] @ 0x%llx => intD 基址 = 0x%llx   邻居签名 %d/%d  sane %d/%d%s ---\n",
         no, IDX_ANCHOR, (unsigned long long)anchor, (unsigned long long)base,
         score, kNeighborN, sane, SANE_NDX,
         (score == kNeighborN && sane > SANE_NDX * 9 / 10) ? "  [最可能]" : "");
  for (size_t i = 0; i < sizeof(kInts) / sizeof(kInts[0]); ++i) {
    int v = 0;
    if (ReadInt(h, base + (uint64_t)kInts[i] * 4, &v)) printf("intD[%d]=%d", kInts[i], v);
    else printf("intD[%d]=<不可读>", kInts[i]);
    fputs(((i + 1) % 8) ? "  " : "\n", stdout);
  }
  printf("\n  phase 70..95 =");
  for (int i = 70; i <= 95; ++i) {
    int v = 0;
    printf(ReadInt(h, base + (uint64_t)i * 4, &v) ? " %d" : " ?", v);
  }
  printf("\n");
}

static int CmpCand(const void *a, const void *b) {
  const Cand *x = (const Cand *)a, *y = (const Cand *)b;
  if (x->sane != y->sane) return y->sane - x->sane;        /* 先看「像脚本变量」的比例 */
  if (x->score != y->score) return y->score - x->score;    /* 再看邻居签名 */
  return (x->base < y->base) ? -1 : (x->base > y->base);   /* 最后按地址升序 */
}

/* 扫描 h 的地址空间，打印前 maxc 条候选；*best_base 回填榜首的基址（0 = 没找到）。 */
static int ScanAndDump(HANDLE h, DWORD pid, int have_anchor, int anchor, int maxc,
                       uint64_t *best_base) {
  SYSTEM_INFO si = {0}; GetSystemInfo(&si);
  uint64_t addr = (uint64_t)(uintptr_t)si.lpMinimumApplicationAddress;
  uint64_t end = (uint64_t)(uintptr_t)si.lpMaximumApplicationAddress;
  const SIZE_T kChunk = 4u << 20;
  unsigned char *buf = (unsigned char *)malloc(kChunk);
  Cand *cands = (Cand *)malloc(sizeof(Cand) * MAX_CANDIDATES);
  if (!buf || !cands) { fprintf(stderr, "内存不足\n"); free(buf); free(cands); return 2; }

  printf("# pid=%lu 模式=%s 打分签名=%d 项\n", (unsigned long)pid,
         have_anchor ? "单值锚（值当 intD[73]）" : "签名窗口（intD[70..76]）", kNeighborN);
  if (!have_anchor) {
    printf("# 签名窗口 =");
    for (int i = 0; i < kSigN; ++i) printf(" %d", kSig[i]);
    printf("  (intD[%d..%d])\n", SIG_IDX0, SIG_IDX0 + kSigN - 1);
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

  printf("# 扫描 %.1f MB，候选 %d 个%s\n", scanned / 1048576.0, n,
         truncated ? "（已截断，只保留前 2 万个）" : "");
  *best_base = n ? cands[0].base : 0;
  int rc = n ? 0 : 1;
  if (!n) {
    printf("没找到：确认游戏停在**小游戏等待画面**（那时 intD[70..76]=20,19,15,10,0,-1,1）。\n");
    printf("若确认停在那儿仍找不到，可试「单值锚」：pt00_probe.exe <进程> 10\n");
  } else {
    int top = (n < maxc) ? n : maxc;
    for (int i = 0; i < top; ++i)
      DumpCandidate(h, cands[i].base, cands[i].score, cands[i].sane, i + 1);
    printf("\n# 取标着 [最可能] 的那条（intD 基址 + 下标*4 即地址）；整段贴回来做 A2 对照。\n");
  }
  free(buf); free(cands);
  return rc;
}

/* 自检：在本进程里摆一份合成的 intD，再拿探针去扫自己，验证「找到 + 基址正确」。 */
static int SelfTest(void) {
  static int fake_intd[4096];
  for (int i = 0; i < 4096; ++i) fake_intd[i] = 100000 + i; /* 先填成别的，避免假命中 */
  for (int i = 0; i < kSigN; ++i) fake_intd[SIG_IDX0 + i] = kSig[i];
  uint64_t expect = (uint64_t)(uintptr_t)&fake_intd[0];
  printf("# selftest: 合成 intD 基址 = 0x%llx\n", (unsigned long long)expect);
  /* 探针自己的 .rdata 里就存着签名常量，扫自己时必然命中它 —— 明确排除掉。 */
  g_excl_lo = (uint64_t)(uintptr_t)&kSig[0];
  g_excl_hi = g_excl_lo + sizeof(kSig) + 64;
  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
  if (!h) { fprintf(stderr, "selftest: OpenProcess(self) 失败 err=%lu\n",
                    (unsigned long)GetLastError()); return 2; }
  uint64_t got = 0;
  int rc = ScanAndDump(h, GetCurrentProcessId(), 0, 0, 3, &got);
  CloseHandle(h);
  int ok = (rc == 0 && got == expect);
  printf("\n# selftest: %s（期望基址 0x%llx，探针给 0x%llx）\n", ok ? "PASS" : "FAIL",
         (unsigned long long)expect, (unsigned long long)got);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: pt00_probe.exe <process name|pid> [anchor_value] [max_candidates=4]\n"
                    "       pt00_probe.exe --selftest\n"
                    "  不给 anchor_value -> 用签名窗口搜 intD[70..76] = 20,19,15,10,0,-1,1\n"
                    "  给了 anchor_value -> 当它是 intD[73]，逐值搜再用邻居签名打分\n");
    return 2;
  }
  if (strcmp(argv[1], "--selftest") == 0) return SelfTest();

  DWORD pid = 0;
  if (argv[1][0] >= '0' && argv[1][0] <= '9') pid = (DWORD)strtoul(argv[1], NULL, 10);
  if (!pid) {
    int enumerated = 0, opened = 0;
    static char seen[64][64]; int seen_n = 0;
    pid = FindPidByName(argv[1], &enumerated, &opened, seen, 64, &seen_n);
    if (!pid) {
      fprintf(stderr, "找不到进程 \"%s\"。（枚举到 %d 个进程，其中 %d 个可打开）\n",
              argv[1], enumerated, opened);
      if (enumerated == 0)
        fprintf(stderr, "  枚举到 0 个 —— 说明当前就是受限账户（沙箱）：请在沙箱外运行本工具。\n");
      else if (opened == 0)
        fprintf(stderr, "  一个都打不开 —— 权限不足：用管理员身份运行，或改用 PID。\n");
      else
        fprintf(stderr, "  名字对不上：试试可执行文件名（含扩展名，如 REALLIVE.EXE），或直接用 PID。\n");
      if (seen_n > 0) {
        fprintf(stderr, "  已看到（前 %d 个）：", seen_n);
        for (int i = 0; i < seen_n; ++i) fprintf(stderr, " %s", seen[i]);
        fprintf(stderr, "\n");
      }
      return 1;
    }
  }

  int have_anchor = (argc > 2);
  int anchor = have_anchor ? atoi(argv[2]) : 0;
  int maxc = (argc > 3) ? atoi(argv[3]) : 4;
  if (maxc < 1) maxc = 1;

  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) {
    fprintf(stderr, "OpenProcess 失败 pid=%lu err=%lu（试试以管理员身份运行）\n",
            (unsigned long)pid, (unsigned long)GetLastError());
    return 1;
  }
  uint64_t best = 0;
  int rc = ScanAndDump(h, pid, have_anchor, anchor, maxc, &best);
  CloseHandle(h);
  return rc;
}
