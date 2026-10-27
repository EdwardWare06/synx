# The Synx runtime, linked into every program the compiler builds.
add_library(synx_rt STATIC synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c synx_cache.c)
set_target_properties(synx_rt PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/runtime
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/runtime)
if(MSVC)
  target_compile_definitions(synx_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Built-in linked executables import this prebuilt capsule. Its loader performs
# C runtime and TLS initialization. Static synx_rt stays the system-link baseline.
add_library(synx_rt_shared SHARED synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c synx_cache.c)
target_compile_definitions(synx_rt_shared PRIVATE SYNX_SHARED_RUNTIME _CRT_SECURE_NO_WARNINGS)
set_target_properties(synx_rt_shared PROPERTIES OUTPUT_NAME synx_rt
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime/import
  WINDOWS_EXPORT_ALL_SYMBOLS ON)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(synx_rt_shared PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/runtime
    LIBRARY_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/runtime
    ARCHIVE_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/runtime/import)
endforeach()
if(MSVC)
  set_target_properties(synx_rt_shared PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_link_libraries(synx_rt_shared PRIVATE shell32 advapi32)
elseif(APPLE)
  set_target_properties(synx_rt_shared PROPERTIES OSX_ARCHITECTURES x86_64
    INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON)
  target_compile_options(synx_rt_shared PRIVATE -mmacosx-version-min=10.15)
  target_link_options(synx_rt_shared PRIVATE -mmacosx-version-min=10.15)
else()
  target_link_libraries(synx_rt_shared PRIVATE pthread)
endif()
# Axon emits x86-64 only for now. On Apple Silicon the runtime is built for
# x86-64 as well and programs run under Rosetta.
if(APPLE)
  set_target_properties(synx_rt PROPERTIES OSX_ARCHITECTURES x86_64)
endif()

# A prelinked native host supplies CRT/TLS startup once per release. The Synx
# linker appends the resolved native image; this executable needs no capsule.
set(SYNX_EXPORT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/generate-runtime-exports.py
  ${CMAKE_CURRENT_SOURCE_DIR} ${SYNX_EXPORT_DIR} COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c synx_cache.c
  ${CMAKE_SOURCE_DIR}/scripts/generate-runtime-exports.py)
add_executable(synx_rt_host synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c synx_cache.c synx_loader.c)
target_compile_definitions(synx_rt_host PRIVATE SYNX_STANDALONE_RUNTIME _CRT_SECURE_NO_WARNINGS)
target_include_directories(synx_rt_host PRIVATE ${SYNX_EXPORT_DIR})
set_target_properties(synx_rt_host PROPERTIES OUTPUT_NAME synx-host RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(synx_rt_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/runtime)
endforeach()
if(MSVC)
  set_target_properties(synx_rt_host PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_link_libraries(synx_rt_host PRIVATE shell32 advapi32)
  target_link_options(synx_rt_host PRIVATE /STACK:8388608 /DYNAMICBASE /NXCOMPAT)
elseif(APPLE)
  set_target_properties(synx_rt_host PROPERTIES OSX_ARCHITECTURES x86_64)
  target_compile_options(synx_rt_host PRIVATE -mmacosx-version-min=10.15)
  target_link_options(synx_rt_host PRIVATE -mmacosx-version-min=10.15 -Wl,-headerpad,0x1000 -Wl,-adhoc_codesign)
else()
  target_compile_options(synx_rt_host PRIVATE -fPIE)
  target_link_options(synx_rt_host PRIVATE -pie -Wl,-z,relro,-z,now,-z,noexecstack)
  target_link_libraries(synx_rt_host PRIVATE pthread)
endif()
