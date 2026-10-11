#include "env_info.h"

#include <gnu/libc-version.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "cache.h"
#include "log.h"
#include "nvml.h"
#include "user_config.h"

#ifndef LTXAG_VERSION
#define LTXAG_VERSION "0.0.0"
#endif
#ifndef LTXAG_GIT
#define LTXAG_GIT "unknown"
#endif
#ifndef LTXAG_DEV_BUILD
#define LTXAG_DEV_BUILD 0
#endif

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

std::vector<std::string> environmentSnapshot(const HostInfo& host, const std::string& resources) {
  std::vector<std::string> out;
  out.push_back(std::string("plugin: LTX Alpha Gen ") + LTXAG_VERSION + " git " + LTXAG_GIT +
                " built " + __DATE__ + " " + __TIME__ + (LTXAG_DEV_BUILD ? " (dev build)" : ""));
  out.push_back("host: " + host.name + " / " + host.label + " version " + host.version +
                (host.background ? " (background render host)" : ""));
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
  const VramInfo v = queryVram();
  out.push_back(v.ok ? "gpu: " + v.name + ", driver " + v.driver + ", " + gb(v.totalGB * 1073741824.0) +
                           " total, " + gb(v.freeGB * 1073741824.0) + " free"
                     : std::string("gpu: NVML unavailable"));
  out.push_back("resources: " + resources);
  out.push_back("log: " + logPath());
  out.push_back("config: " + userConfigPath());
  const UserConfig cfg = loadUserConfig();
  out.push_back("cache: " + cacheSettings(cfg).root);
  for (const auto& kv : cfg)
    if (kv.first.find("token") == std::string::npos)
      out.push_back("config." + kv.first + ": " + kv.second);
  return out;
}

}  // namespace ltxag
