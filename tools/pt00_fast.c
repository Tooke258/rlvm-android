/* pt00_fast.c -- PT00 fast window sampler (READ-ONLY helper for the PC side).
 *
 * Why: the contact instant lives inside a single logic frame (swing ladder is
 * 15 deg per frame, frame ~= 27 ms), so 2 Hz sampling (--entities / --full)
 * always misses the [624] value at contact. This tool reads ONE small window
 * (intD[220..640]) per sample and prints one line, so it can run at 60+ Hz.
 *
 * Usage:
 *   pt00_probe.exe <pid> --entities          (once, to learn the intD base)
 *   pt00_fast.exe  <pid> <intd_base_hex> [--ms 15] [--secs 30]
 *
 * Columns per line (t_ms then the slot groups we need for the contact table):
 *   226..231   ball current/previous position
 *   250..262   ball state (write by pt00_set_ball_velocity)
 *   263..277   [264] = direction code (1000*sin(theta))
 *   610..613   target (home plate z lives in [612])
 *   620..626   box / bat angle ([624] = bat angle, 0.1 deg units)
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: pt00_fast.exe <pid> <intd_base_hex> [--ms 15] [--secs 30]\n");
    return 2;
  }
  DWORD pid = (DWORD)strtoul(argv[1], NULL, 10);
  unsigned long long base = _strtoui64(argv[2], NULL, 16);
  int ms = 15, secs = 30;
  for (int i = 3; i + 1 < argc; ++i) {
    if (!strcmp(argv[i], "--ms")) ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--secs")) secs = atoi(argv[++i]);
  }
  if (ms < 1) ms = 1;

  HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!h) { printf("open process %lu failed (err %lu)\n", pid, GetLastError()); return 1; }

  printf("# pid=%lu base=0x%llx ms=%d secs=%d\n", pid, base, ms, secs);
  printf("# t_ms 226..231 | 250..262 | 263..277 | 610..613 | 620..626\n");
  fflush(stdout);

  const int LO = 220, HI = 640, N = HI - LO + 1;
  int* buf = (int*)malloc(sizeof(int) * (size_t)N);
  if (!buf) return 1;

  LARGE_INTEGER freq, t0, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  for (;;) {
    QueryPerformanceCounter(&now);
    double el = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    if (el > secs) break;
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, (LPCVOID)(base + (unsigned long long)LO * 4),
                           buf, (SIZE_T)(sizeof(int) * (size_t)N), &got) ||
        got != (SIZE_T)(sizeof(int) * (size_t)N)) {
      printf("read failed at t=%.3f (err %lu)\n", el, GetLastError());
      break;
    }
#define AT(slot) buf[(slot) - LO]
    printf("%.3f", el);
    for (int i = 226; i <= 231; ++i) printf(" %d", AT(i));
    printf(" |");
    for (int i = 250; i <= 262; ++i) printf(" %d", AT(i));
    printf(" |");
    for (int i = 263; i <= 277; ++i) printf(" %d", AT(i));
    printf(" |");
    for (int i = 610; i <= 613; ++i) printf(" %d", AT(i));
    printf(" |");
    for (int i = 620; i <= 626; ++i) printf(" %d", AT(i));
    printf("\n");
#undef AT
    fflush(stdout);
    Sleep((DWORD)ms);
  }
  free(buf);
  CloseHandle(h);
  return 0;
}
