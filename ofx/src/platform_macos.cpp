// macOS side of platform.h (decision 64). GPU memory is in platform_macos_metal.mm.
// Verified against the Command Line Tools MacOSX.sdk (macOS 15): confstr(_CS_DARWIN_USER_TEMP_DIR)
// in unistd.h, proc_name in libproc.h, host_statistics64(HOST_VM_INFO64) in mach/mach_host.h and
// mach/host_info.h, vm_statistics64 fields in mach/vm_statistics.h; sysctl names checked with
// `sysctl kern.osproductversion kern.osversion machdep.cpu.brand_string hw.ncpu hw.memsize`.
#include <libproc.h>
#include <mach/mach.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

#include "platform.h"
#include "platform_macos.h"

namespace ltxag {

namespace {
std::string gb(double bytes) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
  return b;
}
}  // namespace

std::string sysctlString(const char* name) {
  char buf[256] = {0};
  size_t len = sizeof buf - 1;
  return sysctlbyname(name, buf, &len, nullptr, 0) == 0 ? buf : "unknown";
}

double availableRamBytes() {
  // free + inactive pages: what can be handed out without compressing or swapping. The SDK
  // notes that speculative pages are already counted in free_count; purgeable pages sit on the
  // active/inactive queues too, so they are not added again.
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                        &count) != KERN_SUCCESS)
    return 0.0;
  return (double(vm.free_count) + double(vm.inactive_count)) * double(vm_kernel_page_size);
}

// Per-user temp folder (/var/folders/.../T). On disk, not RAM-backed like /dev/shm.
std::string stagingDir() {
  char buf[1024] = {0};
  const size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
  std::string dir = (n > 0 && n <= sizeof buf) ? buf : "/tmp";
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir;
}

// macOS has no per-process GPU memory query like NVML (Metal reports only the calling
// process's own allocations), so the preflight lists no other GPU users.
std::vector<GpuProcess> queryGpuProcesses() { return {}; }

std::string processName(unsigned pid) {
  char buf[256] = {0};
  return proc_name(int(pid), buf, sizeof buf) > 0 ? buf : "";
}

std::vector<std::string> platformSnapshot() {
  std::vector<std::string> out;
  utsname u{};
  uname(&u);
  out.push_back("os: macOS " + sysctlString("kern.osproductversion") + " (" +
                sysctlString("kern.osversion") + "), Darwin " + u.release);
  int ncpu = 0;
  size_t len = sizeof ncpu;
  sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0);
  out.push_back("cpu: " + sysctlString("machdep.cpu.brand_string") + ", " +
                std::to_string(ncpu) + " threads");
  uint64_t mem = 0;
  len = sizeof mem;
  sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
  out.push_back("ram: " + gb(double(mem)) + " total, " + gb(availableRamBytes()) + " available");
  struct statvfs vs {};
  if (statvfs(stagingDir().c_str(), &vs) == 0)
    out.push_back("staging: " + stagingDir() + ", " + gb(double(vs.f_blocks) * vs.f_frsize) +
                  " total, " + gb(double(vs.f_bavail) * vs.f_frsize) + " free");
  return out;
}

// TextEdit (`open -e`, see `man open`) has no title argument, so title is unused here.
bool showTextFile(const std::string& /*title*/, const std::string& path, const Env& env,
                  std::string& err) {
  return launchDetached({"/usr/bin/open", "-e", path}, env, err);
}

}  // namespace ltxag
