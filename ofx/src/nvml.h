// Free/total VRAM of GPU 0 via NVML, loaded with dlopen so the plugin has no
// link-time dependency on the driver. Function names and the memory struct
// verified against pynvml on akflame5 (nvmlInitWithFlags,
// nvmlDeviceGetHandleByIndex_v2, nvmlDeviceGetMemoryInfo {total, free, used},
// nvmlDeviceGetName(handle, char*, uint), nvmlSystemGetDriverVersion(char*, uint)).
#pragma once

#include <string>

namespace ltxag {

struct VramInfo {
  bool ok = false;
  double freeGB = 0.0;
  double totalGB = 0.0;
  std::string name;
  std::string driver;
};

VramInfo queryVram();

}  // namespace ltxag
