cmake_minimum_required(VERSION 3.20)
project(synx VERSION 0.1.0 LANGUAGES C CXX)

# C builds the runtime. C++ builds only the test harnesses. The compiler is
# Synx source in selfhost/, built by a previous Synx release (the seed).
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

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(runtime)

# Seed: a Synx compiler from a previous release builds stage 1. Set SYNX_SEED
# to an installed compiler (libexec/synx.exe on Windows, bin/synx elsewhere),
# or leave it empty to download the release pinned in scripts/seed.json.
set(SYNX_SEED "" CACHE FILEPATH "Synx compiler that builds stage 1 (empty: download the pinned release)")
if(SYNX_SEED)
  set(SYNX_SEED_COMPILER "${SYNX_SEED}")
else()
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fetch-seed.py ${CMAKE_BINARY_DIR}/seed
    OUTPUT_VARIABLE SYNX_SEED_COMPILER OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE seed_result)
  if(NOT seed_result EQUAL 0)
    message(FATAL_ERROR "Cannot get the Synx seed compiler. Set SYNX_SEED to an installed Synx compiler.")
  endif()
endif()
if(NOT EXISTS "${SYNX_SEED_COMPILER}")
  message(FATAL_ERROR "SYNX_SEED does not exist: ${SYNX_SEED_COMPILER}")
endif()
message(STATUS "Synx seed: ${SYNX_SEED_COMPILER}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/scripts/seed.json)

# Stage 1 (built by the seed) and stage 2 (built by stage 1). Stage 2 is the
# compiler that the tests use and that the packages install. Both link with
# the built-in linker against the runtime capsule built here, so the compiler
# can use runtime functions that the seed's own runtime does not have.
file(GLOB_RECURSE SYNX_SELFHOST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/selfhost/*.snx ${CMAKE_SOURCE_DIR}/std/*.snx)
set(SYNX_STAGE_DIR ${CMAKE_BINARY_DIR}/selfhost-stages)
set(SYNX_STAGE1 ${SYNX_STAGE_DIR}/synx-stage1${CMAKE_EXECUTABLE_SUFFIX})
set(SYNX_STAGE2 ${SYNX_STAGE_DIR}/synx-stage2${CMAKE_EXECUTABLE_SUFFIX})
set(SYNX_COMPILER ${SYNX_STAGE2})
set(SYNX_STAGE_ENV ${CMAKE_COMMAND} -E env SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std
    SYNX_RUNTIME_LIB= SYNX_LINKER=builtin SYNX_RUNTIME_SHARED=$<TARGET_FILE:synx_rt_shared>)
add_custom_command(OUTPUT ${SYNX_STAGE1}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SYNX_STAGE_DIR}
  COMMAND ${SYNX_STAGE_ENV} ${SYNX_SEED_COMPILER} build ${CMAKE_SOURCE_DIR}/selfhost/main.snx -o ${SYNX_STAGE1} --keep-obj --linker=builtin
  DEPENDS synx_rt_shared ${SYNX_SELFHOST_SOURCES} ${CMAKE_SOURCE_DIR}/scripts/seed.json
  COMMENT "Building the Synx compiler with the seed release (stage 1)" VERBATIM)
add_custom_command(OUTPUT ${SYNX_STAGE2}
  COMMAND ${SYNX_STAGE_ENV} ${SYNX_STAGE1} build ${CMAKE_SOURCE_DIR}/selfhost/main.snx -o ${SYNX_STAGE2} --keep-obj --linker=builtin
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:synx_rt_shared> ${SYNX_STAGE_DIR}/$<TARGET_FILE_NAME:synx_rt_shared>
  DEPENDS ${SYNX_STAGE1} synx_rt_shared
  COMMENT "Building the Synx compiler with itself (stage 2)" VERBATIM)
add_custom_target(synx_compiler ALL DEPENDS ${SYNX_STAGE2})

enable_testing()
add_subdirectory(tests)

# Installed layout: lib/ holds the runtime, std/ the standard library sources,
# examples/workbench the demo program. The shared capsule also sits beside the
# compiler for the OS loader. Windows uses libexec/synx.exe and bin/synx.cmd.
# The package also holds the two license files, and no other documents,
# tests, or build files.
if(WIN32)
  set(SYNX_COMPILER_DIR libexec)
else()
  set(SYNX_COMPILER_DIR bin)
endif()
install(PROGRAMS ${SYNX_STAGE2} DESTINATION ${SYNX_COMPILER_DIR} RENAME synx${CMAKE_EXECUTABLE_SUFFIX})
install(TARGETS synx_rt ARCHIVE DESTINATION lib)
install(FILES $<TARGET_FILE:synx_rt_shared> DESTINATION lib)
install(FILES $<TARGET_FILE:synx_rt_shared> DESTINATION ${SYNX_COMPILER_DIR})
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
