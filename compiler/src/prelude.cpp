// Shared bootstrap prelude. CMake embeds compiler/prelude.snx unchanged.
#include "synx/resolve.hpp"
#include "prelude_embedded.hpp"
namespace synx::sema {
const char* prelude_source() { return embedded_prelude; }
}
