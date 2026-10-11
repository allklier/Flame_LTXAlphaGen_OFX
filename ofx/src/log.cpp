#include "log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "user_config.h"

namespace ltxag {

namespace {
std::atomic<int> gLevel{int(LogLevel::Info)};
thread_local std::string tNode = "-";
thread_local std::string tJob = "-";

const char* levelName(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    default: return "ERROR";
  }
}

bool parseLevel(const std::string& s, LogLevel& out) {
  if (s == "debug") out = LogLevel::Debug;
  else if (s == "info") out = LogLevel::Info;
  else if (s == "warn") out = LogLevel::Warn;
  else if (s == "error") out = LogLevel::Error;
  else return false;
  return true;
}
}  // namespace

std::string logDir() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.cache/ltx-alphagen/logs";
}

std::string logPath() { return logDir() + "/ltx-alphagen.log"; }

void logConfigure() {
  LogLevel l = LogLevel::Info;
  const char* env = std::getenv("LTXAG_LOG_LEVEL");
  if (!(env && parseLevel(env, l))) {
    const UserConfig cfg = loadUserConfig();
    if (cfg.count("log_level")) parseLevel(cfg.at("log_level"), l);
  }
  setLogLevel(l);
}

void setLogLevel(LogLevel level) { gLevel = int(level); }
LogLevel logLevel() { return LogLevel(gLevel.load()); }
bool logEnabled(LogLevel level) { return int(level) >= gLevel.load(); }

void rotateLogs(size_t maxBytes, int keep) {
  struct stat st {};
  const std::string base = logPath();
  if (stat(base.c_str(), &st) != 0 || size_t(st.st_size) < maxBytes) return;
  std::remove((base + "." + std::to_string(keep)).c_str());
  for (int i = keep - 1; i >= 1; --i)
    std::rename((base + "." + std::to_string(i)).c_str(), (base + "." + std::to_string(i + 1)).c_str());
  std::rename(base.c_str(), (base + ".1").c_str());
}

void logAt(LogLevel level, const char* fmt, ...) {
  if (!logEnabled(level)) return;
  char msg[4096];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  const auto now = std::chrono::system_clock::now();
  const std::time_t secs = std::chrono::system_clock::to_time_t(now);
  const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                         .count() % 1000);
  std::tm tm{};
  localtime_r(&secs, &tm);
  char line[4400];
  const int n = std::snprintf(line, sizeof line,
                              "%04d-%02d-%02d %02d:%02d:%02d.%03d %s ofx[%d/%ld] node=%s job=%s | %s\n",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                              tm.tm_sec, ms, levelName(level), int(getpid()),
                              long(syscall(SYS_gettid)), tNode.c_str(), tJob.c_str(), msg);
  if (n <= 0) return;

  // One O_APPEND write per line: whole lines stay intact across processes.
  const std::string path = logPath();
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0 && errno == ENOENT) {
    // First line for this user (or the folder was deleted): create every missing level.
    const std::string dir = logDir();
    for (size_t pos = 1; (pos = dir.find('/', pos)) != std::string::npos; ++pos)
      mkdir(dir.substr(0, pos).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  }
  if (fd < 0) return;
  ssize_t ignored = ::write(fd, line, size_t(std::min(n, int(sizeof line) - 1)));
  (void)ignored;
  ::close(fd);
}

LogScope::LogScope(const std::string& node, const std::string& job)
    : prevNode_(tNode), prevJob_(tJob) {
  tNode = node.empty() ? "-" : node;
  tJob = job.empty() ? prevJob_ : job;
}

LogScope::~LogScope() {
  tNode = prevNode_;
  tJob = prevJob_;
}

}  // namespace ltxag
