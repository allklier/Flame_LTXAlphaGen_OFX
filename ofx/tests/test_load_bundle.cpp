// Loads the built .ofx the way a host does (dlopen + the two OFX entry points)
// and checks the plugin identity. Catches unresolved symbols before Flame does.
// Usage: test_load_bundle <path to LTXAlphaGen.ofx>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>

#include "ofxCore.h"
#include "ofxImageEffect.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  auto count = reinterpret_cast<int (*)()>(dlsym(lib, "OfxGetNumberOfPlugins"));
  auto get = reinterpret_cast<OfxPlugin* (*)(int)>(dlsym(lib, "OfxGetPlugin"));
  if (!count || !get) {
    std::fprintf(stderr, "entry points missing\n");
    return 1;
  }
  const int n = count();
  OfxPlugin* p = n > 0 ? get(0) : nullptr;
  if (n != 1 || !p || std::strcmp(p->pluginIdentifier, "com.janklier.ltx.alphagen") != 0 ||
      std::strcmp(p->pluginApi, kOfxImageEffectPluginApi) != 0) {
    std::fprintf(stderr, "unexpected plugin table (n=%d)\n", n);
    return 1;
  }
  std::printf("test_load_bundle: %s api=%s v%u.%u loaded OK\n", p->pluginIdentifier,
              p->pluginApi, p->pluginVersionMajor, p->pluginVersionMinor);
  return 0;
}
