// macOS GPU memory (decision 64). Apple Silicon has unified memory: total = Metal's
// recommendedMaxWorkingSetSize (what the GPU can use with good performance; MTLDevice.h), free =
// the smaller of that and the system's available RAM (platform_macos.cpp). "driver" is the macOS
// version (the GPU driver ships with the OS). Built with ARC off: the device is released by hand.
#import <Metal/Metal.h>

#include "platform.h"
#include "platform_macos.h"

namespace ltxag {

VramInfo queryVram() {
  VramInfo info;
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) return info;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
    info.ok = true;
    info.totalGB = double(dev.recommendedMaxWorkingSetSize) / kGB;
    const double avail = availableRamBytes() / kGB;
    info.freeGB = avail < info.totalGB ? avail : info.totalGB;
    info.name = dev.name.UTF8String;
    info.driver = "macOS " + sysctlString("kern.osproductversion");
    [dev release];
  }
  return info;
}

}  // namespace ltxag
