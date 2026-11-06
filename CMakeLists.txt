cmake_minimum_required(VERSION 3.20)
project(kvstore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KV_WERROR "Treat compiler warnings as errors" OFF)
option(KV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KV_FUZZ "Build libFuzzer targets (requires Clang)" OFF)

find_package(Threads REQUIRED)

add_library(kv_warnings INTERFACE)
target_compile_options(kv_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Woverloaded-virtual -Wimplicit-fallthrough -Wformat=2
  $<$<BOOL:${KV_WERROR}>:-Werror>)

if(KV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(kvcore STATIC
  src/aof.cpp
  src/commands.cpp
  src/database.cpp
  src/glob.cpp
  src/log.cpp
  src/resp.cpp
  src/server.cpp
  src/siphash.cpp
  src/zset.cpp)
target_include_directories(kvcore PUBLIC src)
target_link_libraries(kvcore PUBLIC Threads::Threads PRIVATE kv_warnings)

add_executable(kvstore src/main.cpp)
target_link_libraries(kvstore PRIVATE kvcore kv_warnings)

include(CTest)
if(BUILD_TESTING)
  add_executable(unit_tests
    tests/unit/main.cpp
    tests/unit/test_dict.cpp
    tests/unit/test_glob.cpp
    tests/unit/test_resp.cpp
    tests/unit/test_siphash.cpp
    tests/unit/test_database.cpp
    tests/unit/test_zset.cpp)
  target_link_libraries(unit_tests PRIVATE kvcore kv_warnings)
  add_test(NAME unit COMMAND unit_tests)
endif()

if(KV_FUZZ)
  add_executable(fuzz_resp fuzz/fuzz_resp.cpp)
  target_compile_options(fuzz_resp PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_resp PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_resp PRIVATE kvcore)
endif()

# Replays a directory of inputs through the fuzz target logic without libFuzzer,
# so the corpus also runs under GCC and in ordinary test builds.
if(BUILD_TESTING)
  add_executable(fuzz_resp_replay fuzz/fuzz_resp.cpp fuzz/replay_main.cpp)
  target_compile_definitions(fuzz_resp_replay PRIVATE KV_FUZZ_REPLAY)
  target_link_libraries(fuzz_resp_replay PRIVATE kvcore)
  add_test(NAME fuzz_corpus COMMAND fuzz_resp_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus)
endif()
