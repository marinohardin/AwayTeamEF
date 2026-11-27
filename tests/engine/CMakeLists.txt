# Unit tests for engine pieces that build without the engine (plain C, libc only).
#   cmake -S tests/engine -B build/engine-tests && cmake --build build/engine-tests
#   ctest --test-dir build/engine-tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(efengine_tests C)
enable_testing()
set(EFCODE ${CMAKE_CURRENT_LIST_DIR}/../../EFAndroid-SP/app/jni/efcode)

add_executable(test_cfg_overlay ${CMAKE_CURRENT_LIST_DIR}/test_cfg_overlay.c ${EFCODE}/qcommon/cfg_overlay.c)
target_include_directories(test_cfg_overlay PRIVATE ${EFCODE}/qcommon ${CMAKE_CURRENT_LIST_DIR}/../launcher)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(test_cfg_overlay PRIVATE -Wall -Wextra)
endif()
add_test(NAME test_cfg_overlay COMMAND test_cfg_overlay)
