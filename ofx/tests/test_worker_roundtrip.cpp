// End-to-end check of the shim <-> worker contract without an OFX host:
// write frames to /dev/shm, run the dummy worker as the plugin does, read the
// mattes back and compare with the expected luminance. Also checks cancel (kill)
// and that the worker exits when its stdin closes.
// Usage: test_worker_roundtrip <python> <dummy_worker.py>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "half.h"
#include "process.h"
#include "transport.h"

using namespace ltxag;

static int failures = 0;
#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                        \
      std::fprintf(stderr, "\n");                               \
      ++failures;                                               \
    }                                                           \
  } while (0)

static bool runWorker(const std::vector<std::string>& argv, std::vector<std::string>& lines,
                      int& status) {
  WorkerProcess w;
  std::string err;
  if (!w.start(argv, cleanEnvironment(), err)) {
    std::fprintf(stderr, "start failed: %s\n", err.c_str());
    return false;
  }
  while (w.readLines(100, lines)) {
  }
  status = w.wait();
  return true;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <python> <dummy_worker.py>\n", argv[0]);
    return 2;
  }
  const std::string py = argv[1], script = argv[2];
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // 1. Round trip: 10 frames of 70x45 (pads to 96x64 and 17 frames inside the worker).
  const uint32_t W = 70, H = 45, N = 10;
  {
    ShmFile in("in"), out("out");
    FrameWriter fw;
    std::string err;
    CHECK(fw.open(in.path(), W, H, N, err), "open: %s", err.c_str());
    std::vector<float> rgb(W * H * 3);
    std::vector<uint16_t> half(rgb.size());
    for (uint32_t f = 0; f < N; ++f) {
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
          for (int c = 0; c < 3; ++c)
            rgb[(y * W + x) * 3 + c] = float((x + 2 * y + 7 * f + 11 * c) % 97) / 96.0f;
      floatToHalf(rgb.data(), half.data(), rgb.size());
      CHECK(fw.writeFrame(half.data(), err), "write: %s", err.c_str());
    }
    fw.close();

    std::vector<std::string> lines;
    int status = 0;
    CHECK(runWorker({py, "-I", "-B", script, "--in", in.path(), "--chunks", "0:10", "--out-prefix", out.path(),
               "--seconds", "0.2"},
                    lines, status),
          "worker start");
    bool done = false;
    for (const auto& l : lines) {
      std::printf("  worker: %s\n", l.c_str());
      if (l == "done") done = true;
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exit status %d", status);
    CHECK(done, "no done line");

    std::vector<uint16_t> mattes;
    const std::string chunk0 = out.path() + "_0.bin";
    const bool read = readMattes(chunk0, W, H, N, mattes, err);
    unlink(chunk0.c_str());
    if (!read) {
      std::fprintf(stderr, "FAIL read: %s\n", err.c_str());
      return 1;
    }
    std::vector<float> m(mattes.size());
    halfToFloat(mattes.data(), m.data(), m.size());
    double maxErr = 0.0;
    for (uint32_t f = 0; f < N; ++f)
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
          float c[3];
          for (int k = 0; k < 3; ++k) c[k] = float((x + 2 * y + 7 * f + 11 * k) % 97) / 96.0f;
          const double want = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
          maxErr = std::max(maxErr, std::fabs(want - m[(size_t(f) * H + y) * W + x]));
        }
    CHECK(maxErr < 2e-3, "luminance mismatch, max error %g", maxErr);
    std::printf("round trip %ux%ux%u: max error %.2e\n", W, H, N, maxErr);
  }

  // 2. Simulated OOM is reported as an error line, not a crash.
  {
    ShmFile in("in"), out("out");
    std::vector<std::string> lines;
    int status = 0;
    runWorker({py, "-I", "-B", script, "--in", in.path(), "--chunks", "0:1", "--out-prefix", out.path(), "--seconds", "0.1",
               "--simulate", "oom"},
              lines, status);
    bool oom = false;
    for (const auto& l : lines)
      if (l.rfind("error oom", 0) == 0) oom = true;
    CHECK(oom, "no 'error oom' line");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2, "oom exit status %d", status);
  }

  // 3. Crash: the worker dies by signal; we only see a status.
  {
    ShmFile in("in"), out("out");
    std::vector<std::string> lines;
    int status = 0;
    runWorker({py, "-I", "-B", script, "--in", in.path(), "--chunks", "0:1", "--out-prefix", out.path(), "--seconds", "0.1",
               "--simulate", "crash"},
              lines, status);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "crash status %d", status);
  }

  // 4. Cancel: a hanging worker is killed promptly.
  {
    ShmFile in("in"), out("out");
    WorkerProcess w;
    std::string err;
    CHECK(w.start({py, "-I", "-B", script, "--in", in.path(), "--chunks", "0:1", "--out-prefix", out.path(), "--seconds", "0.1",
                   "--simulate", "hang"},
                  cleanEnvironment(), err),
          "start: %s", err.c_str());
    std::vector<std::string> lines;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500))
      w.readLines(50, lines);
    w.kill();
    const int status = w.wait();
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "kill status %d", status);
  }

  // 5. Parent gone: closing the worker's stdin makes it exit on its own (status 3).
  {
    ShmFile in("in"), out("out");
    int toChild[2];
    CHECK(pipe(toChild) == 0, "pipe");
    const pid_t pid = fork();
    if (pid == 0) {
      dup2(toChild[0], 0);
      close(toChild[1]);
      execl(py.c_str(), py.c_str(), "-I", "-B", script.c_str(), "--in", in.path().c_str(), "--chunks", "0:1",
            "--out-prefix", out.path().c_str(), "--seconds", "0.1", "--simulate", "hang",
            (char*)nullptr);
      _exit(127);
    }
    close(toChild[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    close(toChild[1]);  // simulate Flame dying
    int status = 0;
    const auto t0 = std::chrono::steady_clock::now();
    pid_t r = 0;
    while ((r = waitpid(pid, &status, WNOHANG)) == 0 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (r == 0) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
    CHECK(r == pid && WIFEXITED(status) && WEXITSTATUS(status) == 3,
          "worker did not exit on stdin EOF (status %d)", status);
  }

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_worker_roundtrip: all checks passed\n");
  return 0;
}
