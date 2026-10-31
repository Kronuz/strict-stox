cmake_minimum_required(VERSION 3.14)
project(strict_stox LANGUAGES CXX)

# --- Library target ---
# strict-stox is header-only and dependency-free: one strict numeric-parsing
# header (strict_stox.hh) over the C strto* family, with nothing but the C++
# standard library behind it. So the target is a pure INTERFACE library that just
# puts the source dir on the consumer's include path.
add_library(strict_stox INTERFACE)
target_include_directories(strict_stox INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(strict_stox INTERFACE cxx_std_20)
# Alias so consumers can link `strict_stox::strict_stox` whether they add this
# via add_subdirectory or FetchContent.
add_library(strict_stox::strict_stox ALIAS strict_stox)

# Only build the test when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(strict_stox_test test/test.cc)
  target_link_libraries(strict_stox_test PRIVATE strict_stox)
  add_test(NAME strict_stox COMMAND strict_stox_test)

  # Differential test against the frozen pre-optimization header.
  add_executable(strict_stox_diff_test test/diff_test.cc)
  target_include_directories(strict_stox_diff_test PRIVATE test)
  target_link_libraries(strict_stox_diff_test PRIVATE strict_stox)
  add_test(NAME strict_stox_diff COMMAND strict_stox_diff_test)

  # Benchmark (not run by ctest). Comparators (std::from_chars, optionally
  # abseil and fast_float when installed) are only linked by this target.
  option(STRICT_STOX_BUILD_BENCH "Build bench/bench.cc" OFF)
  if(STRICT_STOX_BUILD_BENCH)
    add_executable(strict_stox_bench bench/bench.cc)
    target_include_directories(strict_stox_bench PRIVATE test)
    target_link_libraries(strict_stox_bench PRIVATE strict_stox)
    find_package(absl QUIET)
    if(absl_FOUND)
      target_compile_definitions(strict_stox_bench PRIVATE HAVE_ABSL=1)
      target_link_libraries(strict_stox_bench PRIVATE absl::strings)
    endif()
  endif()

  # A runnable tour of the library (not a test): build & run ./build/strict_stox_demo
  add_executable(strict_stox_demo examples/demo.cc)
  target_link_libraries(strict_stox_demo PRIVATE strict_stox)
endif()
