# Tests. Every test uses the stage 2 compiler (SYNX_COMPILER) built from
# selfhost/. The harnesses are C++ or Python programs that start it.

# Checker, borrow checker, lowering, parser, and lexer cases: one .snx file
# per case with its expectation in a header. See tests/fixtures/run.py.
add_test(NAME fixtures COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/fixtures/run.py
         ${SYNX_COMPILER} ${CMAKE_SOURCE_DIR}/tests/fixtures ${CMAKE_SOURCE_DIR}/tests/run/modules)
set_tests_properties(fixtures PROPERTIES TIMEOUT 1800)

add_test(NAME match_coverage COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/patterns/coverage.py
         ${SYNX_COMPILER} ${CMAKE_BINARY_DIR}/match-coverage)
set_tests_properties(match_coverage PROPERTIES TIMEOUT 1200)

add_test(NAME regalloc COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/regalloc/check.py ${SYNX_COMPILER})
set_tests_properties(regalloc PROPERTIES TIMEOUT 300)

add_test(NAME cli COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/cli/test_cli.py
         ${SYNX_COMPILER} ${CMAKE_BINARY_DIR}/runtime)
set_tests_properties(cli PROPERTIES TIMEOUT 1800)

add_test(NAME workflow COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/workflow/test_workflow.py
         ${SYNX_COMPILER} ${CMAKE_BINARY_DIR}/runtime)
set_tests_properties(workflow PROPERTIES TIMEOUT 1800)

add_test(NAME lsp COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/lsp/test_lsp.py ${SYNX_COMPILER})
set_tests_properties(lsp PROPERTIES TIMEOUT 1800)

add_test(NAME fmt COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/fmt/test_fmt.py
         ${SYNX_COMPILER} $<TARGET_FILE:synx_rt_host>)
set_tests_properties(fmt PROPERTIES TIMEOUT 1800)

add_test(NAME myelin COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/myelin/test_myelin.py ${SYNX_COMPILER})
set_tests_properties(myelin PROPERTIES TIMEOUT 1800)

# Fixed point: stage 2 builds stage 3, and the two compiler objects must be
# byte-identical. Then stage 3 builds and runs the workbench example.
add_test(NAME bootstrap COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/bootstrap/fixed_point.py
         ${SYNX_COMPILER} ${CMAKE_BINARY_DIR}/bootstrap)
set_tests_properties(bootstrap PROPERTIES TIMEOUT 3600)

# End to end: build and run programs with the compiler and runtime. The
# default links with the built-in linker; system_* use the host linker and
# the static runtime.
add_executable(test_run run_test.cpp)
set(SYNX_MODES 0 1 2 3)
foreach(mode ${SYNX_MODES})
  set(flags)
  if(mode EQUAL 1 OR mode EQUAL 3)
    list(APPEND flags --no-regalloc)
  endif()
  if(mode GREATER_EQUAL 2)
    list(APPEND flags --release)
  endif()
  foreach(corpus run examples)
    if(corpus STREQUAL run)
      set(dir ${CMAKE_SOURCE_DIR}/tests/run)
    else()
      set(dir ${CMAKE_SOURCE_DIR}/examples)
    endif()
    add_test(NAME ${corpus}_${mode} COMMAND test_run ${SYNX_COMPILER} ${dir} ${flags})
    add_test(NAME system_${corpus}_${mode} COMMAND test_run ${SYNX_COMPILER} ${dir} ${flags})
    set_tests_properties(${corpus}_${mode} PROPERTIES TIMEOUT 1200
      ENVIRONMENT "SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std;SYNX_LINKER=builtin;SYNX_RUNTIME_SHARED=$<TARGET_FILE:synx_rt_shared>")
    set_tests_properties(system_${corpus}_${mode} PROPERTIES TIMEOUT 1200
      ENVIRONMENT "SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std;SYNX_LINKER=system;SYNX_RUNTIME_LIB=$<TARGET_FILE:synx_rt>")
  endforeach()
  # One probe compilation per mode covers all binary readers and image writers.
  add_test(NAME linker_${mode} COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/tests/linker/suite.py ${SYNX_COMPILER}
    ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} $<TARGET_FILE:synx_rt>
    $<TARGET_FILE:synx_rt_shared> ${mode})
  set_tests_properties(linker_${mode} PROPERTIES TIMEOUT 1200)
endforeach()

# Every example must lex, parse, and check without errors.
file(GLOB SYNX_EXAMPLES ${CMAKE_SOURCE_DIR}/examples/*.snx)
foreach(example ${SYNX_EXAMPLES})
  get_filename_component(name ${example} NAME_WE)
  add_test(NAME lex_example_${name} COMMAND ${SYNX_COMPILER} lex ${example})
  add_test(NAME parse_example_${name} COMMAND ${SYNX_COMPILER} parse ${example})
  add_test(NAME check_example_${name} COMMAND ${SYNX_COMPILER} check ${example})
  set_tests_properties(check_example_${name} PROPERTIES ENVIRONMENT "SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std")
endforeach()

add_executable(test_host host_test.cpp)
add_test(NAME host COMMAND test_host ${SYNX_COMPILER} ${CMAKE_SOURCE_DIR}/tests/run/host.snx)
set_tests_properties(host PROPERTIES TIMEOUT 120
  ENVIRONMENT "SYNX_STD_PATH=${CMAKE_SOURCE_DIR}/std;SYNX_RUNTIME_SHARED=$<TARGET_FILE:synx_rt_shared>")

add_executable(test_host_runtime host_runtime_test.c)
target_link_libraries(test_host_runtime PRIVATE synx_rt)
if(WIN32)
  target_link_options(test_host_runtime PRIVATE /ENTRY:wmainCRTStartup)
endif()
if(APPLE)
  set_target_properties(test_host_runtime PROPERTIES OSX_ARCHITECTURES x86_64)
endif()
add_test(NAME host_runtime COMMAND test_host_runtime)
set_tests_properties(host_runtime PROPERTIES TIMEOUT 30)

# Wake race regression: a runtime variant with a test hook forces the waker
# drop between the executor's readiness and retention checks.
add_library(test_wake_rt STATIC ../runtime/synx_rt.c ../runtime/synx_host.c ../runtime/synx_driver.c task_wake_race/hook.c)
if(MSVC)
  target_compile_definitions(test_wake_rt PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(APPLE)
  set_target_properties(test_wake_rt PROPERTIES OSX_ARCHITECTURES x86_64)
endif()
add_executable(test_task_wake_race task_wake_race/runner.cpp)
add_dependencies(test_task_wake_race test_wake_rt)
foreach(mode ${SYNX_MODES})
  add_test(NAME task_wake_race_${mode}
           COMMAND test_task_wake_race ${SYNX_COMPILER} ${CMAKE_SOURCE_DIR} $<TARGET_FILE:test_wake_rt> ${mode})
  set_tests_properties(task_wake_race_${mode} PROPERTIES TIMEOUT 180)
endforeach()

# Tests run the compiler, so build it with the harnesses.
foreach(harness test_run test_host test_task_wake_race)
  add_dependencies(${harness} synx_compiler)
endforeach()

# The harnesses accept paths through main(argc, argv). Keep the CRT argument
# encoding and std::filesystem's narrow path encoding in UTF-8 on Windows.
if(MSVC)
  get_property(synx_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(synx_test_target IN LISTS synx_test_targets)
    get_target_property(synx_test_type ${synx_test_target} TYPE)
    if(synx_test_type STREQUAL "EXECUTABLE")
      target_sources(${synx_test_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/utf8.manifest")
    endif()
  endforeach()
endif()
