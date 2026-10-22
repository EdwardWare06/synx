#pragma once

namespace synx {

// Returns the compiler version string, for example "0.1.0".
const char* version();

// Returns the host platform name: "windows", "linux", or "macos".
const char* host_platform();

}  // namespace synx
