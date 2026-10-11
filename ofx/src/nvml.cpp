#include "nvml.h"

#include <dlfcn.h>

#include <algorithm>
#include <fstream>
#include <map>

namespace ltxag {

namespace {
struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};
using InitFn = int (*)(unsigned int);
using HandleFn = int (*)(unsigned int, void**);
using MemFn = int (*)(void*, NvmlMemory*);
using ShutdownFn = int (*)();
using NameFn = int (*)(void*, char*, unsigned int);
using DriverFn = int (*)(char*, unsigned int);
struct NvmlProcessInfo {
  unsigned int pid;
  unsigned long long usedGpuMemory;
  unsigned int gpuInstanceId;
  unsigned int computeInstanceId;
};
using ProcFn = int (*)(void*, unsigned int*, NvmlProcessInfo*);
constexpr unsigned long long kNotAvailable = ~0ULL;  // NVML_VALUE_NOT_AVAILABLE_ulonglong
constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
}  // namespace

VramInfo queryVram() {
  VramInfo info;
  void* lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return info;
  auto init = reinterpret_cast<InitFn>(dlsym(lib, "nvmlInitWithFlags"));
  auto handle = reinterpret_cast<HandleFn>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
  auto mem = reinterpret_cast<MemFn>(dlsym(lib, "nvmlDeviceGetMemoryInfo"));
  auto shutdown = reinterpret_cast<ShutdownFn>(dlsym(lib, "nvmlShutdown"));
  auto name = reinterpret_cast<NameFn>(dlsym(lib, "nvmlDeviceGetName"));
  auto driver = reinterpret_cast<DriverFn>(dlsym(lib, "nvmlSystemGetDriverVersion"));
  if (init && handle && mem && shutdown && init(0) == 0) {
    void* dev = nullptr;
    NvmlMemory m{};
    if (handle(0, &dev) == 0 && mem(dev, &m) == 0) {
      info.ok = true;
      info.freeGB = double(m.free) / kGB;
      info.totalGB = double(m.total) / kGB;
      char buf[128] = {0};
      if (name && name(dev, buf, sizeof buf) == 0) info.name = buf;
      buf[0] = 0;
      if (driver && driver(buf, sizeof buf) == 0) info.driver = buf;
    }
    shutdown();
  }
  dlclose(lib);
  return info;
}

std::vector<GpuProcess> queryGpuProcesses() {
  std::vector<GpuProcess> out;
  void* lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return out;
  auto init = reinterpret_cast<InitFn>(dlsym(lib, "nvmlInitWithFlags"));
  auto handle = reinterpret_cast<HandleFn>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
  auto shutdown = reinterpret_cast<ShutdownFn>(dlsym(lib, "nvmlShutdown"));
  ProcFn lists[] = {reinterpret_cast<ProcFn>(dlsym(lib, "nvmlDeviceGetComputeRunningProcesses_v3")),
                    reinterpret_cast<ProcFn>(dlsym(lib, "nvmlDeviceGetGraphicsRunningProcesses_v3"))};
  if (init && handle && shutdown && init(0) == 0) {
    void* dev = nullptr;
    std::map<unsigned, double> used;
    if (handle(0, &dev) == 0) {
      for (ProcFn list : lists) {
        if (!list) continue;
        NvmlProcessInfo infos[128];
        unsigned int count = 128;
        if (list(dev, &count, infos) != 0) continue;
        for (unsigned i = 0; i < count && i < 128; ++i) {
          const double gb = infos[i].usedGpuMemory == kNotAvailable ? -1.0 : double(infos[i].usedGpuMemory) / kGB;
          auto it = used.find(infos[i].pid);
          if (it == used.end() || gb > it->second) used[infos[i].pid] = gb;
        }
      }
    }
    shutdown();
    for (const auto& u : used) {
      GpuProcess p;
      p.pid = u.first;
      p.usedGB = u.second;
      std::ifstream comm("/proc/" + std::to_string(u.first) + "/comm");
      std::getline(comm, p.name);
      if (p.name.empty()) p.name = "pid " + std::to_string(u.first);
      out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const GpuProcess& a, const GpuProcess& b) { return a.usedGB > b.usedGB; });
  }
  dlclose(lib);
  return out;
}

}  // namespace ltxag
