/* 在 PC 上单独跑一遍 **APK 用的那个解码器**（vendored xclannad GRPCONV），
 * 回答「这张图到底解出来是什么」——对象名/矩形都对但像素是空的时候，先排除解码器。
 *
 * 用法: g00_decode_probe.exe <a.g00> [b.g00 ...]
 * 输出: 尺寸 / mask / 子图数 / 每张子图的矩形与「着色像素数(alpha>0)」/ 整图统计
 * 构建: tools\build_g00_decode_probe.bat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "xclannad/file.h"

static std::vector<char> ReadAll(const char* path, bool* ok) {
  std::vector<char> out;
  FILE* f = fopen(path, "rb");
  if (!f) { *ok = false; return out; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    out.resize((size_t)n);
    if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) out.clear();
  }
  fclose(f);
  *ok = !out.empty();
  return out;
}

static const char* BaseName(const char* p) {
  const char* b = strrchr(p, '\\');
  if (!b) b = strrchr(p, '/');
  return b ? b + 1 : p;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: g00_decode_probe.exe <a.g00> [b.g00 ...]\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    bool ok = false;
    std::vector<char> data = ReadAll(argv[i], &ok);
    printf("=== %s (%zu bytes) ===\n", BaseName(argv[i]), data.size());
    if (!ok) { printf("  <cannot read>\n"); ++bad; continue; }

    // 头几个字节：type / w / h / head_size，出问题时最容易看
    if (data.size() >= 9) {
      unsigned int type = (unsigned char)data[0];
      int w = (unsigned char)data[1] | ((unsigned char)data[2] << 8);
      int h = (unsigned char)data[3] | ((unsigned char)data[4] << 8);
      int head = (unsigned char)data[5] | ((unsigned char)data[6] << 8);
      printf("  header: type=%u w=%d h=%d field5=%d\n", type, w, h, head);
    }

    GRPCONV* conv = GRPCONV::AssignConverter(data.data(), (int)data.size(), BaseName(argv[i]));
    if (!conv) { printf("  <AssignConverter failed: unsupported format>\n"); ++bad; continue; }
    const int w = conv->Width(), h = conv->Height();
    printf("  converter: %dx%d mask=%d patterns=%zu\n", w, h, conv->IsMask() ? 1 : 0,
           conv->region_table.size());
    if (w <= 0 || h <= 0) { printf("  <bad dimensions>\n"); delete conv; ++bad; continue; }

    std::vector<unsigned char> img((size_t)w * h * 4, 0);
    if (!conv->Read((char*)img.data())) {
      printf("  <Read() FAILED>\n");
      delete conv;
      ++bad;
      continue;
    }
    size_t inked = 0, opaque = 0;
    for (size_t p = 0; p < img.size(); p += 4) {
      const unsigned int a = img[p + 3];
      if (a > 0) ++inked;
      if (a > 128) ++opaque;
    }
    printf("  whole image: inked(a>0)=%zu  opaque(a>128)=%zu  of %d px  (%.1f%% inked)\n",
           inked, opaque, w * h, 100.0 * (double)inked / (double)std::max(1, w * h));

    for (size_t r = 0; r < conv->region_table.size(); ++r) {
      const GRPCONV::REGION& g = conv->region_table[r];
      size_t ri = 0, ro = 0;
      for (int y = g.y1; y <= g.y2 && y < h; ++y) {
        for (int x = g.x1; x <= g.x2 && x < w; ++x) {
          if (x < 0 || y < 0) continue;
          const unsigned int a = img[((size_t)y * w + x) * 4 + 3];
          if (a > 0) ++ri;
          if (a > 128) ++ro;
        }
      }
      printf("    patt[%zu] rect=(%d,%d)-(%d,%d) origin=(%d,%d) inked=%zu opaque=%zu\n",
             r, g.x1, g.y1, g.x2, g.y2, g.origin_x, g.origin_y, ri, ro);
    }
    delete conv;
  }
  return bad ? 1 : 0;
}
