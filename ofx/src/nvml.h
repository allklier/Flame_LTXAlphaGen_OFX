// Free/total VRAM of GPU 0 via NVML, loaded with dlopen so the plugin has no
// link-time dependency on the driver. Function names and the memory struct
// verified against pynvml on the reference machine (nvmlInitWithFlags,
// nvmlDeviceGetHandleByIndex_v2, nvmlDeviceGetMemoryInfo {total, free, used},
// nvmlDeviceGetName(handle, char*, uint), nvmlSystemGetDriverVersion(char*, uint)).
// Per-process use: nvmlDeviceGet{Compute,Graphics}RunningProcesses_v3(handle, uint* count,
// nvmlProcessInfo_t*), struct {uint pid; ull usedGpuMemory; uint gpuInstanceId; uint
// computeInstanceId} (layout checked against nvidia-smi on the reference machine, tools/nvml_probe.cpp).
#pragma once

#include <string>
#include <vector>

namespace ltxag {

struct VramInfo {
  bool ok = false;
  double freeGB = 0.0;
  double totalGB = 0.0;
  std::string name;
  std::string driver;
};

VramInfo queryVram();

struct GpuProcess {
  unsigned pid = 0;
  double usedGB = 0.0;  // < 0: the driver did not report it
  std::string name;     // /proc/<pid>/comm
};

// Compute and graphics processes on GPU 0, merged by pid, largest first. Empty if NVML fails.
std::vector<GpuProcess> queryGpuProcesses();

}  // namespace ltxag
