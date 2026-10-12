#include "env_info.h"

#include <cstdio>

#include "cache.h"
#include "log.h"
#include "platform.h"
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
  for (const auto& line : platformSnapshot()) out.push_back(line);
  const VramInfo v = queryVram();
  out.push_back(v.ok ? "gpu: " + v.name + ", driver " + v.driver + ", " + gb(v.totalGB * 1073741824.0) +
                           " total, " + gb(v.freeGB * 1073741824.0) + " free"
                     : std::string("gpu: memory query unavailable"));
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
