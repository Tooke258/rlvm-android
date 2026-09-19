#include "android/log_redirect.h"

#include <android/log.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace rlvm_android {
namespace {

constexpr char kStdoutTag[] = "rlvm-stdout";
constexpr char kStderrTag[] = "rlvm-stderr";

// logcat 对单条消息有长度上限（约 1000 字符，见 docs/PROGRESS.md 的踩坑记录）。
// 长行必须切片发送，否则尾部会被静默丢弃。
constexpr std::size_t kMaxLogChunk = 800;

// 没有换行的超长输出（例如把一段数据整个打到 cerr）也要定期送出，
// 否则管道内积累的字节看不到。
constexpr std::size_t kMaxPending = 4096;

// 每个流一份读取状态。管道读端 + 标签 + 优先级。
struct StreamEndpoint {
  int read_fd = -1;
  const char* tag = nullptr;
  int priority = ANDROID_LOG_INFO;
};

/** 把一行（可能超长）按 logcat 限制切片送出。 */
void EmitLine(const StreamEndpoint& endpoint, const std::string& line) {
  if (line.empty()) {
    __android_log_print(endpoint.priority, endpoint.tag, " ");
    return;
  }

  std::size_t begin = 0;
  while (begin < line.size()) {
    const std::size_t take =
        (line.size() - begin < kMaxLogChunk) ? (line.size() - begin) : kMaxLogChunk;
    __android_log_print(endpoint.priority, endpoint.tag, "%.*s",
                        static_cast<int>(take), line.c_str() + begin);
    begin += take;
  }
}

/** 读取线程：把管道里的字节按行转发到 logcat。 */
void PumpStream(StreamEndpoint endpoint) {
  std::string pending;
  pending.reserve(kMaxPending);
  char chunk[1024];

  for (;;) {
    const ssize_t count = read(endpoint.read_fd, chunk, sizeof(chunk));
    if (count < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (count == 0) break;  // 写端已全部关闭

    for (ssize_t i = 0; i < count; ++i) {
      const char c = chunk[i];
      if (c == '\n') {
        // 去掉行尾 \r，CRLF 输出在 logcat 里会留下多余字符。
        if (!pending.empty() && pending.back() == '\r') pending.pop_back();
        EmitLine(endpoint, pending);
        pending.clear();
        continue;
      }
      pending.push_back(c);
      if (pending.size() >= kMaxPending) {
        EmitLine(endpoint, pending);
        pending.clear();
      }
    }
  }

  if (!pending.empty()) EmitLine(endpoint, pending);
}

/**
 * 单个 fd 的重定向。
 *
 * 流程：建管道 -> 先把 C 流里缓冲的内容刷给原 fd（否则会丢）-> dup2 把目标 fd
 * 换成管道写端 -> 目标流改成行缓冲（引擎用 std::endl 或 "\n" 输出时能及时进 logcat）。
 */
void RedirectFd(int target_fd, const char* tag, int priority) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kStdoutTag,
                        "pipe() failed for fd %d: errno=%d", target_fd, errno);
    return;
  }

  // 先刷旧缓冲，避免 glibc/libc++ 里已有的内容被丢掉。
  fflush(nullptr);

  if (dup2(pipe_fds[1], target_fd) < 0) {
    __android_log_print(ANDROID_LOG_ERROR, kStdoutTag,
                        "dup2() failed for fd %d: errno=%d", target_fd, errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }
  close(pipe_fds[1]);

  FILE* stream = (target_fd == STDOUT_FILENO) ? stdout : stderr;
  static char stdout_buffer[4096];
  static char stderr_buffer[4096];
  setvbuf(stream, (target_fd == STDOUT_FILENO) ? stdout_buffer : stderr_buffer,
          _IOLBF, sizeof(stdout_buffer));

  StreamEndpoint endpoint;
  endpoint.read_fd = pipe_fds[0];
  endpoint.tag = tag;
  endpoint.priority = priority;
  std::thread(PumpStream, endpoint).detach();
}

}  // namespace

void InstallLogRedirect() {
  static std::once_flag once;
  std::call_once(once, [] {
    RedirectFd(STDOUT_FILENO, kStdoutTag, ANDROID_LOG_INFO);
    RedirectFd(STDERR_FILENO, kStderrTag, ANDROID_LOG_WARN);
  });
}

}  // namespace rlvm_android
