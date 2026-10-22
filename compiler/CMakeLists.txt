# Keep the native oracle and Synx port on the same prelude bytes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prelude.snx")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/prelude.snx" SYNX_PRELUDE)
configure_file(prelude_embedded.hpp.in prelude_embedded.hpp @ONLY)

# The Synx compiler. Library plus a thin command line driver so tests can
# link the library directly.

add_library(synx_core
  src/version.cpp
  src/source.cpp
  src/diagnostics.cpp
  src/token.cpp
  src/lexer.cpp
  src/ast_dump.cpp
  src/ast_strict.cpp
  src/parser.cpp
  src/types.cpp
  src/resolve.cpp
  src/prelude.cpp
  src/check.cpp
  src/compile.cpp
  src/modules.cpp
  src/module_dump.cpp
  src/ir_print.cpp
  src/lower.cpp
  src/borrow.cpp
  src/async.cpp
  src/layout.cpp
  src/x64.cpp
  src/objfile.cpp
  src/axon.cpp
  src/regalloc.cpp
  src/build.cpp
)
target_include_directories(synx_core PUBLIC include)

add_executable(synx src/main.cpp)
target_link_libraries(synx PRIVATE synx_core)

# Development fallback; installed builds first search beside their executable.
target_compile_definitions(synx_core PRIVATE SYNX_STD_DIR="${CMAKE_SOURCE_DIR}/std")

target_include_directories(synx_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
