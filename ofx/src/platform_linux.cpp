// Linux side of platform.h (decision 64). GPU memory is in platform_linux_nvml.cpp.
#include <gnu/libc-version.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "platform.h"

namespace ltxag {

namespace {
std::string firstMatch(const char* path, const std::string& prefix) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
  return "unknown";
}

std::string gb(double bytes) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
  return b;
}
}  // namespace

std::string stagingDir() { return "/dev/shm"; }

std::string processName(unsigned pid) {
  std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return name;
}

std::vector<std::string> platformSnapshot() {
  std::vector<std::string> out;
  utsname u{};
  uname(&u);
  std::string os = firstMatch("/etc/os-release", "PRETTY_NAME=");
  out.push_back("os: " + os + ", kernel " + u.release + ", glibc " + gnu_get_libc_version());
  out.push_back("cpu: " + firstMatch("/proc/cpuinfo", "model name\t: ") + ", " +
                std::to_string(sysconf(_SC_NPROCESSORS_ONLN)) + " threads");
  struct sysinfo si {};
  sysinfo(&si);
  out.push_back("ram: " + gb(double(si.totalram) * si.mem_unit) + " total, " +
                gb(double(si.freeram) * si.mem_unit) + " free");
  struct statvfs vs {};
  if (statvfs("/dev/shm", &vs) == 0)
    out.push_back("dev/shm: " + gb(double(vs.f_blocks) * vs.f_frsize) + " total, " +
                  gb(double(vs.f_bavail) * vs.f_frsize) + " free");
  return out;
}

bool showTextFile(const std::string& title, const std::string& path, const Env& env,
                  std::string& err) {
  return launchDetached({"/usr/bin/zenity", "--text-info", "--title=" + title,
                         "--filename=" + path, "--width=640", "--height=520"},
                        env, err);
}

}  // namespace ltxag
