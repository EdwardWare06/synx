# The Synx runtime, linked into every program the compiler builds.
add_library(synx_rt STATIC synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c)
set_target_properties(synx_rt PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/runtime
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/runtime)
if(MSVC)
  target_compile_definitions(synx_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Built-in linked executables import this prebuilt capsule. Its loader performs
# C runtime and TLS initialization. Static synx_rt stays the system-link baseline.
add_library(synx_rt_shared SHARED synx_rt.c synx_threads.c synx_host.c synx_driver.c synx_link.c)
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
  target_link_libraries(synx_rt_shared PRIVATE shell32)
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
