/* PT00 原生对照 / oracle（32 位控制台程序）。
 *
 * 用途：把原版 PT00.dll 直接跑起来，喂给它一串调用，记录每步对 intD 的改动。
 * 之后拿这个轨迹当"真值"，与我们自己的实现（或用执行器跑的同一份 DLL）逐项对比。
 *
 * 为什么只需要这么点代码：PT00.dll 只导入 KERNEL32，和引擎的全部交互就是
 *    reallive_dll_func_load(ctx, a2)     —— ctx 里偏移 +0x14 处放着 intD 基址
 *    reallive_dll_func_call(func,a1..a4) —— 脚本通过 intD 读写数据
 * 这两件事都已实测验证（见 docs/LB-MINIGAME-RETRO.md §4）。
 *
 * 用法：
 *   oracle.exe <PT00.dll 路径> <调用脚本.txt> [快照输出.bin] [--seed 种子快照.bin]
 *
 * 调用脚本每行一次调用：  func a1 a2 a3 a4        （十进制或 0x 十六进制，后四个可省）
 * 以 '#' 开头或空行忽略。
 *
 * `--seed` 用来从二进制快照恢复初始状态（格式与输出一致：intD[2000] + intF[2000]，
 * 全是 32 位小端）。对照必须从「同一起点」开始，否则两边的差异都是噪声。
 *
 * 每次调用后打印一行：调用序号、func、参数、返回值、以及 intD 里发生变化的槽位。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#error "This oracle is a 32-bit Windows program."
#endif

#define INTD_COUNT 2000
#define INTF_COUNT 2000

typedef int(__stdcall *func_load_t)(void *ctx, int a2);
typedef int(__stdcall *func_free_t)(void);
typedef int(__stdcall *func_init_t)(void);
typedef int(__stdcall *func_call_t)(int func, int a1, int a2, int a3, int a4);

/* 引擎上下文：DLL 只从 +0x14 取 intD 基址，其余留空即可。 */
static struct {
  void *slots[32];
} g_ctx;

static int g_intd[INTD_COUNT];
static int g_intf[INTF_COUNT];

static int parse_int(const char *s) {
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s + 2, NULL, 16);
  return (int)strtol(s, NULL, 10);
}

/* 让 oracle 与执行器**共用同一个确定性时间源**。
 *
 * 原因：PT00 的 `func_load` 开头是 `time(&t); srand(t);`，而 `func 71`
 * （实体类型行为）里用 `rand()`。oracle 拿真实时间、emu 的 GetSystemTime 桩返回全 0，
 * 两边随机序列从第一次 rand 起就分叉 —— 对照里表现为「只有 func 71 不一致」。
 * 这里把 IAT 里的时间类导入改写成确定性桩，两边才可比。 */
static uint32_t __stdcall StubReturnsZeroPtr(void *p) {
  if (p) memset(p, 0, 16); /* SYSTEMTIME 正好 16 字节；别越界写坏调用者的栈 */
  return 0;
}

static void patch_iat(HMODULE mod, const char *name, void *fn) {
  unsigned char *base = (unsigned char *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
  IMAGE_DATA_DIRECTORY dd =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dd.VirtualAddress) return;
  IMAGE_IMPORT_DESCRIPTOR *imp =
      (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress);
  for (; imp->Name; ++imp) {
    uint32_t oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                           : imp->FirstThunk;
    IMAGE_THUNK_DATA32 *ilt = (IMAGE_THUNK_DATA32 *)(base + oft);
    IMAGE_THUNK_DATA32 *iat = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
    for (int i = 0; ilt[i].u1.AddressOfData; ++i) {
      if (ilt[i].u1.Ordinal & 0x80000000u) continue;
      IMAGE_IMPORT_BY_NAME *ibn =
          (IMAGE_IMPORT_BY_NAME *)(base + ilt[i].u1.AddressOfData);
      if (strcmp((const char *)ibn->Name, name) == 0) {
        DWORD old = 0;
        VirtualProtect(&iat[i].u1.Function, sizeof(void *), PAGE_READWRITE, &old);
        iat[i].u1.Function = (uint32_t)(uintptr_t)fn;
        VirtualProtect(&iat[i].u1.Function, sizeof(void *), old, &old);
        return;
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: oracle <PT00.dll> <calls.txt> [out.bin] [--seed seed.bin]\n");
    return 2;
  }

  const char *seed_path = NULL;
  const char *out_path = NULL;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed_path = argv[++i];
    } else if (!out_path) {
      out_path = argv[i];
    }
  }

  HMODULE mod = LoadLibraryA(argv[1]);
  if (!mod) {
    fprintf(stderr, "LoadLibrary failed: %lu\n", (unsigned long)GetLastError());
    return 1;
  }

  func_load_t p_load = (func_load_t)GetProcAddress(mod, "reallive_dll_func_load");
  func_free_t p_free = (func_free_t)GetProcAddress(mod, "reallive_dll_func_free");
  func_init_t p_init = (func_init_t)GetProcAddress(mod, "reallive_dll_func_init");
  func_call_t p_call = (func_call_t)GetProcAddress(mod, "reallive_dll_func_call");
  if (!p_load || !p_call) {
    fprintf(stderr, "missing exports (load=%p call=%p)\n", (void *)p_load,
            (void *)p_call);
    return 1;
  }

  /* 时间源确定性：与执行器（GetSystemTime/GetLocalTime/GetTimeZoneInformation
   * 全返回 0）对齐，否则 srand 的种子不同 → rand 序列不同 → 只有用 rand 的
   * func 71 会对不上。 */
  patch_iat(mod, "GetSystemTime", (void *)StubReturnsZeroPtr);
  patch_iat(mod, "GetLocalTime", (void *)StubReturnsZeroPtr);
  patch_iat(mod, "GetTimeZoneInformation", (void *)StubReturnsZeroPtr);

  memset(&g_ctx, 0, sizeof(g_ctx));
  memset(g_intd, 0, sizeof(g_intd));
  memset(g_intf, 0, sizeof(g_intf));
  *(void **)((char *)&g_ctx + 0x14) = g_intd; /* 引擎上下文里的 intD 基址 */

  if (seed_path) {
    FILE *s = fopen(seed_path, "rb");
    if (!s) {
      fprintf(stderr, "cannot open seed file: %s\n", seed_path);
      return 1;
    }
    size_t n1 = fread(g_intd, sizeof(int), INTD_COUNT, s);
    size_t n2 = fread(g_intf, sizeof(int), INTF_COUNT, s);
    fclose(s);
    printf("# seed %s: intd read=%u intf read=%u\n", seed_path,
           (unsigned)n1, (unsigned)n2);
    printf("# seed check: intD[70..76]=");
    for (int i = 70; i <= 76; ++i) printf("%s%d", i == 70 ? "" : ",", g_intd[i]);
    printf("  intD[73](mode)=%d\n", g_intd[73]);
  }

  int ret_load = p_load(&g_ctx, 0);
  printf("# func_load -> %d   (intd=%p)\n", ret_load, (void *)g_intd);
  { /* CRT 的 rand 状态：ptd+0x14（ptd 从 DLL 自己的 TLS 索引取）——与 emu 的同类打印对齐 */
    uint32_t idx = *(uint32_t *)((char *)mod + 0x20AA0);
    void *ptd = TlsGetValue((DWORD)idx);
    if (ptd) *(int *)((char *)ptd + 0x14) = 1; /* 与 emu 对齐：钉死 rand 起点 */
    printf("# TLS idx=%08x ptd=%p holdrand=%d\n", idx, ptd,
           ptd ? *(int *)((char *)ptd + 0x14) : -1);
  }
  if (p_init) printf("# func_init -> %d\n", p_init());

  FILE *f = fopen(argv[2], "r");
  if (!f) {
    fprintf(stderr, "cannot open calls file: %s\n", argv[2]);
    return 1;
  }

  char line[512];
  int n = 0;
  int prev[INTD_COUNT];
  memcpy(prev, g_intd, sizeof(g_intd));
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
    int func = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
    char tok[5][64];
    int got = sscanf(p, "%63s %63s %63s %63s %63s", tok[0], tok[1], tok[2], tok[3],
                     tok[4]);
    if (got < 1) continue;
    func = parse_int(tok[0]);
    if (got > 1) a1 = parse_int(tok[1]);
    if (got > 2) a2 = parse_int(tok[2]);
    if (got > 3) a3 = parse_int(tok[3]);
    if (got > 4) a4 = parse_int(tok[4]);

    int ret = p_call(func, a1, a2, a3, a4);
    ++n;
    printf("%d func=%d(%d,%d,%d,%d) ret=%d", n, func, a1, a2, a3, a4, ret);
    for (int i = 0; i < INTD_COUNT; ++i) {
      if (g_intd[i] != prev[i]) printf(" intD[%d]=%d", i, g_intd[i]);
    }
    printf("\n");
    memcpy(prev, g_intd, sizeof(g_intd));
  }
  fclose(f);

  if (out_path) {
    FILE *o = fopen(out_path, "wb");
    if (o) {
      fwrite(g_intd, sizeof(int), INTD_COUNT, o);
      fwrite(g_intf, sizeof(int), INTF_COUNT, o);
      fclose(o);
      printf("# intd+intf snapshot -> %s\n", out_path);
    }
  }

  if (p_free) printf("# func_free -> %d\n", p_free());
  return 0;
}
