cmake_minimum_required(VERSION 3.20)
project(synx VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(runtime)
add_subdirectory(compiler)

enable_testing()
add_subdirectory(tests)

# Self-hosted compiler: the C++ compiler (stage 0) builds selfhost/main.snx
# into stage 1, and stage 1 builds stage 2. The two objects must be identical.
# Target synx_selfhost is not part of ALL; scripts/package-* build it.
file(GLOB_RECURSE SYNX_SELFHOST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/selfhost/*.snx ${CMAKE_SOURCE_DIR}/std/*.snx)
set(SYNX_STAGE_DIR ${CMAKE_BINARY_DIR}/selfhost-stages)
set(SYNX_STAGE1 ${SYNX_STAGE_DIR}/synx-stage1${CMAKE_EXECUTABLE_SUFFIX})
set(SYNX_STAGE2 ${SYNX_STAGE_DIR}/synx-stage2${CMAKE_EXECUTABLE_SUFFIX})
if(WIN32)
  set(SYNX_OBJECT_SUFFIX .obj)
else()
  set(SYNX_OBJECT_SUFFIX .o)
endif()
set(SYNX_STAGE_ENV ${CMAKE_COMMAND} -E env SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std
    SYNX_RUNTIME_LIB=$<TARGET_FILE:synx_rt> SYNX_LINKER=system)
# Build a temporary system-linked helper, then use it to produce the installed
# helper with the built-in linker and shared runtime capsule.
set(SYNX_LINKER_HELPER ${CMAKE_BINARY_DIR}/compiler/synx-link${CMAKE_EXECUTABLE_SUFFIX})
set(SYNX_LINKER_STAGE0 ${CMAKE_BINARY_DIR}/compiler/synx-link-stage0${CMAKE_EXECUTABLE_SUFFIX})
if(WIN32)
  set(SYNX_LINKER_OS windows)
elseif(APPLE)
  set(SYNX_LINKER_OS macos)
else()
  set(SYNX_LINKER_OS linux)
endif()
add_custom_command(OUTPUT ${SYNX_LINKER_HELPER}
  BYPRODUCTS ${SYNX_LINKER_STAGE0} ${SYNX_LINKER_STAGE0}${SYNX_OBJECT_SUFFIX}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/compiler
  COMMAND ${SYNX_STAGE_ENV} $<TARGET_FILE:synx> build ${CMAKE_SOURCE_DIR}/selfhost/link_driver.snx
          -o ${SYNX_LINKER_STAGE0} --keep-obj --linker=system
  COMMAND ${SYNX_LINKER_STAGE0} ${SYNX_LINKER_OS} ${SYNX_LINKER_STAGE0}${SYNX_OBJECT_SUFFIX}
          ${SYNX_LINKER_HELPER} $<TARGET_FILE:synx_rt_shared>
  DEPENDS synx synx_rt synx_rt_shared ${SYNX_SELFHOST_SOURCES} ${CMAKE_SOURCE_DIR}/compiler/prelude.snx
  COMMENT "Building the Synx built-in linker helper" VERBATIM)
add_custom_target(synx_linker ALL DEPENDS ${SYNX_LINKER_HELPER})
add_dependencies(test_run synx_linker synx_rt_shared)
add_dependencies(test_selfhost_driver synx_rt_shared)
add_dependencies(test_selfhost_bootstrap synx_linker synx_rt_shared)
add_custom_command(OUTPUT ${SYNX_STAGE1}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SYNX_STAGE_DIR}
  COMMAND ${SYNX_STAGE_ENV} $<TARGET_FILE:synx> build ${CMAKE_SOURCE_DIR}/selfhost/main.snx -o ${SYNX_STAGE1} --keep-obj --linker=system
  DEPENDS synx synx_rt ${SYNX_SELFHOST_SOURCES} ${CMAKE_SOURCE_DIR}/compiler/prelude.snx
  COMMENT "Building the Synx compiler with the C++ compiler (stage 1)" VERBATIM)
add_custom_command(OUTPUT ${SYNX_STAGE2}
  COMMAND ${CMAKE_COMMAND} -E env SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std
          SYNX_RUNTIME_LIB= SYNX_LINKER=builtin SYNX_RUNTIME_SHARED=$<TARGET_FILE:synx_rt_shared>
          ${SYNX_STAGE1} build ${CMAKE_SOURCE_DIR}/selfhost/main.snx -o ${SYNX_STAGE2} --keep-obj --linker=builtin
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:synx_rt_shared> ${SYNX_STAGE_DIR}/$<TARGET_FILE_NAME:synx_rt_shared>
  COMMAND ${CMAKE_COMMAND} -E compare_files ${SYNX_STAGE1}${SYNX_OBJECT_SUFFIX} ${SYNX_STAGE2}${SYNX_OBJECT_SUFFIX}
  DEPENDS ${SYNX_STAGE1} synx_rt_shared
  COMMENT "Building the Synx compiler with itself (stage 2)" VERBATIM)
add_custom_target(synx_selfhost DEPENDS ${SYNX_STAGE2})

# Installed layout: lib/ holds the runtime, std/ the standard library sources,
# examples/workbench the demo program. The shared capsule also sits beside the
# compiler for the OS loader. Windows uses libexec/synx.exe and bin/synx.cmd.
# The package also holds the two license files, and no other documents,
# tests, or build files.
set(SYNX_INSTALL_COMPILER "synx" CACHE STRING
    "Compiler installed as synx: synx (self-hosted stage 2, default) or cpp (C++ stage 0)")
set_property(CACHE SYNX_INSTALL_COMPILER PROPERTY STRINGS synx cpp)
if(NOT SYNX_INSTALL_COMPILER MATCHES "^(synx|cpp)$")
  message(FATAL_ERROR "SYNX_INSTALL_COMPILER must be synx or cpp")
endif()
if(WIN32)
  set(SYNX_COMPILER_DIR libexec)
else()
  set(SYNX_COMPILER_DIR bin)
endif()
if(SYNX_INSTALL_COMPILER STREQUAL "cpp")
  install(TARGETS synx RUNTIME DESTINATION ${SYNX_COMPILER_DIR})
else()
  install(CODE "if(NOT EXISTS \"${SYNX_STAGE2}\")
  message(FATAL_ERROR \"Build target synx_selfhost before installing the self-hosted compiler\")
endif()")
  install(PROGRAMS ${SYNX_STAGE2} DESTINATION ${SYNX_COMPILER_DIR} RENAME synx${CMAKE_EXECUTABLE_SUFFIX})
endif()
install(TARGETS synx_rt ARCHIVE DESTINATION lib)
install(FILES $<TARGET_FILE:synx_rt_shared> DESTINATION lib)
install(FILES $<TARGET_FILE:synx_rt_shared> DESTINATION ${SYNX_COMPILER_DIR})
if(SYNX_INSTALL_COMPILER STREQUAL "cpp")
  install(CODE "if(NOT EXISTS \"${SYNX_LINKER_HELPER}\")
  message(FATAL_ERROR \"Build target synx_linker before installing\")
endif()")
  install(PROGRAMS ${SYNX_LINKER_HELPER} DESTINATION libexec)
  install(FILES $<TARGET_FILE:synx_rt_shared> DESTINATION libexec)
endif()
install(DIRECTORY std/ DESTINATION std FILES_MATCHING PATTERN "*.snx")
install(FILES examples/workbench/main.snx examples/workbench/calculator.snx DESTINATION examples/workbench)
install(FILES LICENSE-MIT LICENSE-APACHE DESTINATION .)
if(WIN32)
  install(PROGRAMS packaging/windows/synx.cmd DESTINATION bin)
  install(FILES packaging/windows/install.ps1 DESTINATION .)
  install(PROGRAMS packaging/windows/install.cmd DESTINATION .)
else()
  install(PROGRAMS packaging/unix/install.sh DESTINATION .)
endif()
