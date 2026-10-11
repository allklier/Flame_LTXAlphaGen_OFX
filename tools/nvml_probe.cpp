// Prints queryVram() and queryGpuProcesses() so they can be compared with nvidia-smi.
//   g++ -std=c++14 -I ofx/src tools/nvml_probe.cpp ofx/src/nvml.cpp -ldl -o nvml_probe
#include <cstdio>

#include "nvml.h"

int main() {
  const ltxag::VramInfo v = ltxag::queryVram();
  std::printf("ok=%d free=%.2f total=%.2f %s\n", v.ok, v.freeGB, v.totalGB, v.name.c_str());
  for (const auto& p : ltxag::queryGpuProcesses())
    std::printf("pid %u %s %.0f MiB\n", p.pid, p.name.c_str(), p.usedGB * 1024.0);
  return 0;
}
