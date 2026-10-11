// Per-user config: ~/.config/ltx-alphagen/config.conf, plain "key=value" lines.
// Holds the user defaults of decision 21 (precision, headroom_gb, unload) and
// paths. Absent keys fall back to the built-in defaults.
#pragma once

#include <map>
#include <string>

namespace ltxag {

using UserConfig = std::map<std::string, std::string>;

std::string userConfigPath();
UserConfig loadUserConfig();
bool saveUserConfig(const UserConfig& cfg);  // atomic replace

}  // namespace ltxag
