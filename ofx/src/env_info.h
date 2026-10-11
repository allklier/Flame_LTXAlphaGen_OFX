// Environment snapshot for the log and the support bundle (decision 39).
#pragma once

#include <string>
#include <vector>

namespace ltxag {

struct HostInfo {
  std::string name, label, version;
  bool background = false;
};

// One "key: value" line per fact. No secrets: config keys containing "token" are skipped.
std::vector<std::string> environmentSnapshot(const HostInfo& host, const std::string& resources);

}  // namespace ltxag
