# Tests. Each phase adds its own test executable here.

include("${CMAKE_CURRENT_SOURCE_DIR}/stage7.cmake")

add_executable(test_version version_test.cpp)
target_link_libraries(test_version PRIVATE synx_core)
add_test(NAME version COMMAND test_version)

add_executable(test_lexer lexer_test.cpp)
target_link_libraries(test_lexer PRIVATE synx_core)
add_test(NAME lexer COMMAND test_lexer)

add_executable(test_parser parser_test.cpp)
target_link_libraries(test_parser PRIVATE synx_core)
add_test(NAME parser COMMAND test_parser)

add_executable(test_ast_strict ast_strict_test.cpp)
target_link_libraries(test_ast_strict PRIVATE synx_core)
add_test(NAME ast_strict COMMAND test_ast_strict)

# Parser parity: the Synx parser in selfhost/ must produce the same strict
# dump, token position, and diagnostics as the C++ parser. One ctest entry per
# compiler build mode (default, no register allocation, release, both).
add_executable(test_selfhost_parser selfhost_parser_test.cpp)
target_link_libraries(test_selfhost_parser PRIVATE synx_core)
if(MSVC)
  target_link_options(test_selfhost_parser PRIVATE /STACK:8388608)
endif()
foreach(mode RANGE 0 3)
  add_test(NAME selfhost_parser_${mode}
           COMMAND test_selfhost_parser $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${mode})
  set_tests_properties(selfhost_parser_${mode} PROPERTIES TIMEOUT 900)
endforeach()

add_executable(test_selfhost_types selfhost_types_test.cpp)
target_link_libraries(test_selfhost_types PRIVATE synx_core)
add_executable(test_selfhost_backend selfhost_backend_test.cpp)
target_link_libraries(test_selfhost_backend PRIVATE synx_core)
add_executable(test_selfhost_modules selfhost_modules_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(test_selfhost_modules PRIVATE synx_core Threads::Threads)
foreach(mode RANGE 0 3)
  add_test(NAME selfhost_types_${mode}
           COMMAND test_selfhost_types $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${mode})
  add_test(NAME selfhost_backend_${mode}
           COMMAND test_selfhost_backend $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${mode})
  set_tests_properties(selfhost_backend_${mode} PROPERTIES TIMEOUT 900)
  # One probe build and one compilation per entry cover every ported pass:
  # declarations, signatures, checker tables, lowering, ownership checks, and
  # async frame conversion.
  add_test(NAME selfhost_frontend_${mode}
           COMMAND test_selfhost_modules $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} ${mode} all
                   $<TARGET_FILE:test_borrow> $<TARGET_FILE:test_async_pool>)
  set_tests_properties(selfhost_types_${mode} selfhost_frontend_${mode} PROPERTIES TIMEOUT 1800)
endforeach()

add_executable(test_check check_test.cpp)
target_link_libraries(test_check PRIVATE synx_core)
add_test(NAME check COMMAND test_check)

add_executable(test_ir ir_test.cpp)
target_link_libraries(test_ir PRIVATE synx_core)
target_compile_definitions(test_ir PRIVATE SYNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ir COMMAND test_ir)

add_executable(test_borrow borrow_test.cpp)
target_link_libraries(test_borrow PRIVATE synx_core)
add_test(NAME borrow COMMAND test_borrow)

# End to end: build and run programs with the real compiler and runtime.
add_executable(test_run run_test.cpp)
target_link_libraries(test_run PRIVATE synx_core)
add_test(NAME run COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/tests/run)
add_test(NAME run_examples COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/examples)
add_test(NAME run_stack COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/tests/run --no-regalloc)
add_test(NAME run_examples_stack COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/examples --no-regalloc)
add_test(NAME run_release COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/tests/run --release)
add_test(NAME run_examples_release COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/examples --release)
add_test(NAME run_stack_release COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/tests/run --no-regalloc --release)
add_test(NAME run_examples_stack_release COMMAND test_run $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/examples --no-regalloc --release)
set_tests_properties(run run_examples run_stack run_examples_stack run_release run_examples_release run_stack_release run_examples_stack_release PROPERTIES TIMEOUT 600)

# One probe compilation per mode covers all binary readers and image writers.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(mode RANGE 0 3)
  add_test(NAME linker_${mode} COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/tests/linker/suite.py $<TARGET_FILE:synx>
    ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} $<TARGET_FILE:synx_rt>
    $<TARGET_FILE:synx_rt_shared> ${mode})
  set_tests_properties(linker_${mode} PROPERTIES TIMEOUT 1200)
  set(linker_flags)
  if(mode EQUAL 1 OR mode EQUAL 3)
    list(APPEND linker_flags --no-regalloc)
  endif()
  if(mode GREATER_EQUAL 2)
    list(APPEND linker_flags --release)
  endif()
  foreach(corpus run examples)
    if(corpus STREQUAL run)
      set(linker_corpus ${CMAKE_SOURCE_DIR}/tests/run)
    else()
      set(linker_corpus ${CMAKE_SOURCE_DIR}/examples)
    endif()
    add_test(NAME builtin_${corpus}_${mode} COMMAND test_run $<TARGET_FILE:synx> ${linker_corpus} ${linker_flags})
    set_tests_properties(builtin_${corpus}_${mode} PROPERTIES TIMEOUT 1200
      ENVIRONMENT "SYNX_LINKER=builtin;SYNX_RUNTIME_SHARED=$<TARGET_FILE:synx_rt_shared>")
  endforeach()
endforeach()

add_executable(test_regalloc regalloc_test.cpp)
target_link_libraries(test_regalloc PRIVATE synx_core)
add_test(NAME regalloc COMMAND test_regalloc)

# Every example must lex, parse, and check without errors.
file(GLOB SYNX_EXAMPLES ${CMAKE_SOURCE_DIR}/examples/*.snx)
foreach(example ${SYNX_EXAMPLES})
  get_filename_component(name ${example} NAME_WE)
  add_test(NAME lex_example_${name} COMMAND synx lex ${example})
  add_test(NAME parse_example_${name} COMMAND synx parse ${example})
  add_test(NAME check_example_${name} COMMAND synx check ${example})
endforeach()

add_executable(test_modules module_test.cpp)
target_link_libraries(test_modules PRIVATE synx_core)
target_compile_definitions(test_modules PRIVATE SYNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME modules COMMAND test_modules)

add_executable(test_collections collections_test.cpp)
target_link_libraries(test_collections PRIVATE synx_core)
add_test(NAME collections COMMAND test_collections)

add_executable(test_stdlib stdlib_test.cpp)
target_link_libraries(test_stdlib PRIVATE synx_core)
add_test(NAME stdlib COMMAND test_stdlib)

add_executable(test_threads threads_test.cpp)
target_link_libraries(test_threads PRIVATE synx_core)
add_test(NAME threads COMMAND test_threads)

add_executable(test_async_pool async_pool_test.cpp)
target_link_libraries(test_async_pool PRIVATE synx_core)
add_test(NAME async_pool COMMAND test_async_pool)

add_executable(test_host host_test.cpp)
add_test(NAME host COMMAND test_host $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR}/tests/run/host.snx)
set_tests_properties(host PROPERTIES TIMEOUT 120)

add_executable(test_host_runtime host_runtime_test.c)
target_link_libraries(test_host_runtime PRIVATE synx_rt)
if(WIN32)
  target_link_options(test_host_runtime PRIVATE /ENTRY:wmainCRTStartup)
endif()
if(APPLE)
  set_target_properties(test_host_runtime PROPERTIES OSX_ARCHITECTURES x86_64)
endif()
add_test(NAME host_runtime COMMAND test_host_runtime)

add_executable(test_selfhost_lexer selfhost_lexer_test.cpp)
target_link_libraries(test_selfhost_lexer PRIVATE synx_core)
add_test(NAME selfhost_lexer COMMAND test_selfhost_lexer $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR})
set_tests_properties(selfhost_lexer PROPERTIES TIMEOUT 300)
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
target_link_libraries(test_task_wake_race PRIVATE synx_core)
add_dependencies(test_task_wake_race test_wake_rt)
foreach(mode RANGE 0 3)
  add_test(NAME task_wake_race_${mode}
           COMMAND test_task_wake_race $<TARGET_FILE:synx> ${CMAKE_SOURCE_DIR} $<TARGET_FILE:test_wake_rt> ${mode})
  set_tests_properties(task_wake_race_${mode} PROPERTIES TIMEOUT 180)
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
