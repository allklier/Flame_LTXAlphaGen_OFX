// Helpers shared by platform_macos.cpp and platform_macos_metal.mm (decision 64).
#pragma once

#include <string>

namespace ltxag {

std::string sysctlString(const char* name);  // "unknown" if the sysctl fails
double availableRamBytes();                  // free + inactive pages, 0 if the query fails

}  // namespace ltxag
