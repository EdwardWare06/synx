#include "synx/version.hpp"

namespace synx {

const char* version() { return "0.1.0"; }

const char* host_platform() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

}  // namespace synx
