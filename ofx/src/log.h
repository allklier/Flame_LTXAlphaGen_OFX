// Diagnostic logging (decision 39). One rotating per-user log shared by every
// component (ofx, job, worker, helper):  ~/.cache/ltx-alphagen/logs/ltx-alphagen.log
// Line format (must match worker/ltxag_worker/log.py):
//   2026-10-10 16:35:53.383 INFO  ofx[166761/166761] node=ab12cd34 job=- | message
// Logging never throws and never blocks rendering; a failed write is dropped.
#pragma once

#include <string>

namespace ltxag {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

std::string logDir();
std::string logPath();

// Level from $LTXAG_LOG_LEVEL, else the user config key log_level, else info.
void logConfigure();
void setLogLevel(LogLevel level);
LogLevel logLevel();
bool logEnabled(LogLevel level);

// Keeps at most `keep` rotated files of about `maxBytes` each. Called at plugin load.
void rotateLogs(size_t maxBytes = 10u << 20, int keep = 5);

void logAt(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGD(...) ::ltxag::logAt(::ltxag::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::ltxag::logAt(::ltxag::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::ltxag::logAt(::ltxag::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::ltxag::logAt(::ltxag::LogLevel::Error, __VA_ARGS__)

// Correlation ids for every line logged on this thread while the scope lives.
class LogScope {
 public:
  LogScope(const std::string& node, const std::string& job = "");
  ~LogScope();
  LogScope(const LogScope&) = delete;
  LogScope& operator=(const LogScope&) = delete;

 private:
  std::string prevNode_, prevJob_;
};

}  // namespace ltxag
