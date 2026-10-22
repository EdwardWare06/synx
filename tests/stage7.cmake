# Included by tests/CMakeLists.txt. Driver core comparisons stay independent
# from the full compiler and the optional three-build fixed-point check.
add_executable(test_selfhost_driver selfhost_driver_test.cpp)
target_link_libraries(test_selfhost_driver PRIVATE synx_core)
add_executable(test_selfhost_bootstrap selfhost_bootstrap_test.cpp)
target_link_libraries(test_selfhost_bootstrap PRIVATE synx_core)
foreach(mode RANGE 0 3)
  add_test(NAME selfhost_driver_${mode}
           COMMAND test_selfhost_driver $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${mode})
  set_tests_properties(selfhost_driver_${mode} PROPERTIES TIMEOUT 900)
endforeach()
add_test(NAME selfhost_cli
         COMMAND test_selfhost_driver $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} 0 cli)
set_tests_properties(selfhost_cli PROPERTIES TIMEOUT 3600)
option(SYNX_ENABLE_BOOTSTRAP_TESTS "Run the complete Synx compiler bootstrap and stage-2 suites" OFF)
if(SYNX_ENABLE_BOOTSTRAP_TESTS)
  add_test(NAME selfhost_bootstrap
           COMMAND test_selfhost_bootstrap $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  set_tests_properties(selfhost_bootstrap PROPERTIES TIMEOUT 21600 RUN_SERIAL TRUE)
endif()
