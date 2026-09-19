#include "android/saf_file_system.h"

#include <unistd.h>

namespace rlvm_android {
namespace {

std::shared_ptr<SafBackend> g_backend;

}  // namespace

void SetSafBackend(std::shared_ptr<SafBackend> backend) {
  g_backend = std::move(backend);
}

std::shared_ptr<SafBackend> GetSafBackend() { return g_backend; }

bool SafReadAll(const std::string& rel_path, std::string& out) {
  std::shared_ptr<SafBackend> backend = GetSafBackend();
  if (!backend) return false;

  const int fd = backend->OpenFd(rel_path);
  if (fd < 0) return false;

  out.clear();
  char buffer[8192];
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      close(fd);
      out.clear();
      return false;
    }
    if (n == 0) break;
    out.append(buffer, static_cast<size_t>(n));
  }
  close(fd);
  return true;
}

}  // namespace rlvm_android
