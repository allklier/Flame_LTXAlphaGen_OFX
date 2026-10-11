#include "nvml.h"

#include <dlfcn.h>

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

}  // namespace ltxag
