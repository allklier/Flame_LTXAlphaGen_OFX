// Everything that differs between operating systems (decision 64). The rest of the plugin is
// portable POSIX/OFX code and calls only these. Linux: platform_linux.cpp and
// platform_linux_nvml.cpp, selected in CMakeLists.txt.
#pragma once

#include <string>
#include <vector>

#include "process.h"

namespace ltxag {

// Folder for the frame files exchanged with the job and worker (RAM-backed on Linux:
// /dev/shm). No trailing '/'. worker/job.py takes it from its manifest's folder.
std::string stagingDir();

// GPU 0 memory as seen by the driver.
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
  std::string name;     // processName(pid)
};
// Processes using GPU 0, merged by pid, largest first. Empty if the query fails.
std::vector<GpuProcess> queryGpuProcesses();

// Short process name ("flame"), empty if the process is gone.
std::string processName(unsigned pid);

// OS-specific lines of the environment snapshot (os, cpu, ram, staging space).
std::vector<std::string> platformSnapshot();

// Opens a read-only window showing a text file (the licence), detached from Flame.
bool showTextFile(const std::string& title, const std::string& path, const Env& env,
                  std::string& err);

}  // namespace ltxag
