#include "user_config.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace ltxag {

std::string userConfigPath() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.config/ltx-alphagen/config.conf";
}

UserConfig loadUserConfig() {
  UserConfig cfg;
  std::ifstream in(userConfigPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    cfg[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return cfg;
}

bool saveUserConfig(const UserConfig& cfg) {
  const std::string path = userConfigPath();
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# LTX Alpha Gen OFX user settings. Written by the plugin.\n";
    for (const auto& kv : cfg) out << kv.first << '=' << kv.second << '\n';
    if (!out) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace ltxag
