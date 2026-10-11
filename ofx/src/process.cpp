#include "process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;

namespace ltxag {

namespace {

const char* const kStripped[] = {"PYTHONHOME=",    "PYTHONPATH=",  "PYTHONSTARTUP=",
                                 "PYTHONUSERBASE=", "LD_LIBRARY_PATH=", "LD_PRELOAD=",
                                 "HF_TOKEN="};

std::vector<char*> cstrs(const std::vector<std::string>& v) {
  std::vector<char*> out;
  for (const auto& s : v) out.push_back(const_cast<char*>(s.c_str()));
  out.push_back(nullptr);
  return out;
}

// Shared spawn setup: default signal dispositions, empty mask, no inherited fds.
struct SpawnSetup {
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  SpawnSetup(short extraFlags) {
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | extraFlags);
  }
  ~SpawnSetup() {
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
  }
};

}  // namespace

Env cleanEnvironment(const Env& extras) {
  Env env;
  for (char** e = environ; e && *e; ++e) {
    bool drop = false;
    for (const char* prefix : kStripped)
      if (std::strncmp(*e, prefix, std::strlen(prefix)) == 0) drop = true;
    if (!drop) env.emplace_back(*e);
  }
  env.insert(env.end(), extras.begin(), extras.end());
  return env;
}

WorkerProcess::~WorkerProcess() {
  if (pid_ > 0) {
    kill();
    wait();
  }
  if (outFd_ >= 0) close(outFd_);
  if (inFd_ >= 0) close(inFd_);
}

bool WorkerProcess::start(const std::vector<std::string>& argv, const Env& env,
                          std::string& err) {
  int out[2], in[2];
  if (pipe2(out, O_CLOEXEC) != 0 || pipe2(in, O_CLOEXEC) != 0) {
    err = std::string("pipe: ") + std::strerror(errno);
    return false;
  }
  SpawnSetup s(POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&s.attr, 0);  // own process group
  posix_spawn_file_actions_adddup2(&s.fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&s.fa, out[1], 1);
  posix_spawn_file_actions_adddup2(&s.fa, out[1], 2);
  posix_spawn_file_actions_addclosefrom_np(&s.fa, 3);

  auto args = cstrs(argv);
  auto envp = cstrs(env);
  const int rc = posix_spawn(&pid_, argv[0].c_str(), &s.fa, &s.attr, args.data(), envp.data());
  close(out[1]);
  close(in[0]);
  outFd_ = out[0];
  inFd_ = in[1];
  if (rc != 0) {
    pid_ = -1;
    err = "cannot start " + argv[0] + ": " + std::strerror(rc);
    return false;
  }
  return true;
}

bool WorkerProcess::readLines(int timeoutMs, std::vector<std::string>& lines) {
  if (outFd_ < 0) return false;
  pollfd p{outFd_, POLLIN, 0};
  const int r = poll(&p, 1, timeoutMs);
  if (r > 0) {
    char chunk[4096];
    const ssize_t k = read(outFd_, chunk, sizeof chunk);
    if (k > 0) {
      buf_.append(chunk, size_t(k));
    } else if (k == 0 || (errno != EINTR && errno != EAGAIN)) {
      if (!buf_.empty()) lines.push_back(buf_);
      buf_.clear();
      close(outFd_);
      outFd_ = -1;
    }
  }
  size_t nl;
  while ((nl = buf_.find('\n')) != std::string::npos) {
    lines.push_back(buf_.substr(0, nl));
    buf_.erase(0, nl + 1);
  }
  return outFd_ >= 0;
}

void WorkerProcess::kill() {
  if (pid_ > 0) ::kill(-pid_, SIGKILL);
}

int WorkerProcess::wait() {
  int status = 0;
  if (pid_ > 0) {
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
  }
  return status;
}

bool launchDetached(const std::vector<std::string>& argv, const Env& env, std::string& err) {
  // sh backgrounds the real command and exits at once, so the helper is
  // reparented to init and we only ever reap the short-lived shell.
  std::vector<std::string> shArgv = {"/bin/sh", "-c",
                                     "\"$0\" \"$@\" </dev/null >/dev/null 2>&1 &"};
  shArgv.insert(shArgv.end(), argv.begin(), argv.end());
  SpawnSetup s(POSIX_SPAWN_SETSID);
  posix_spawn_file_actions_addclosefrom_np(&s.fa, 0);
  auto args = cstrs(shArgv);
  auto envp = cstrs(env);
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, "/bin/sh", &s.fa, &s.attr, args.data(), envp.data());
  if (rc != 0) {
    err = std::string("cannot start helper: ") + std::strerror(rc);
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return true;
}

}  // namespace ltxag
