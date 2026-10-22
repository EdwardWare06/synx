// Smoke test: the core library links and reports a version.

#include <cstdio>
#include <cstring>

#include "synx/version.hpp"

int main() {
  if (std::strcmp(synx::version(), "0.1.0") != 0) {
    std::puts("version mismatch");
    return 1;
  }
  const char* host = synx::host_platform();
  if (std::strcmp(host, "unknown") == 0) {
    std::puts("unknown host platform");
    return 1;
  }
  std::printf("ok: synx %s on %s\n", synx::version(), host);
  return 0;
}
