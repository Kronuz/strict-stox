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
# stod/stof ask the C library for the locale's decimal point before they read a '.',
# like strtod does (a libc call as costly as parsing a short number). A program that
# never leaves the "C" locale can skip it: plain decimals are then read with '.' in
# every locale, and strtod still decides everything else.
option(STRICT_STOX_C_LOCALE "Read '.' as the decimal point whatever the locale says" OFF)
if(STRICT_STOX_C_LOCALE)
  target_compile_definitions(strict_stox INTERFACE STRICT_STOX_C_LOCALE=1)
endif()
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

  # Arbitrary-length input against libc strto* and std::from_chars.
  add_executable(strict_stox_long_test test/long_test.cc)
  target_link_libraries(strict_stox_long_test PRIVATE strict_stox)
  add_test(NAME strict_stox_long COMMAND strict_stox_long_test)

  # The floating point fast path against libc strtod/strtof and std::from_chars: hard cases,
  # exact halfway points of every binade, random texts, the 5^q table against big integers.
  # `strict_stox_fp_test --stress N` and `--f32` are the long runs (not in ctest).
  find_package(Threads)
  add_executable(strict_stox_fp_test test/fp_test.cc)
  target_link_libraries(strict_stox_fp_test PRIVATE strict_stox)
  if(Threads_FOUND)
    target_link_libraries(strict_stox_fp_test PRIVATE Threads::Threads)
  endif()
  add_test(NAME strict_stox_fp COMMAND strict_stox_fp_test)
  # The same code without the 128 bit multiply, for compilers that lack __int128.
  add_executable(strict_stox_fp_portable_test test/fp_test.cc)
  target_compile_definitions(strict_stox_fp_portable_test PRIVATE STRICT_STOX_PORTABLE_MUL64=1)
  target_link_libraries(strict_stox_fp_portable_test PRIVATE strict_stox)
  if(Threads_FOUND)
    target_link_libraries(strict_stox_fp_portable_test PRIVATE Threads::Threads)
  endif()
  add_test(NAME strict_stox_fp_portable COMMAND strict_stox_fp_portable_test)

  # stod/stof under every rounding mode (fesetround), against libc: the fast path stands aside in all
  # but the default mode.
  add_executable(strict_stox_rounding_test test/rounding_test.cc)
  target_link_libraries(strict_stox_rounding_test PRIVATE strict_stox)
  if(Threads_FOUND)
    target_link_libraries(strict_stox_rounding_test PRIVATE Threads::Threads)
  endif()
  add_test(NAME strict_stox_rounding COMMAND strict_stox_rounding_test)

  # The exact decimal path: stod/stof reach libc only for what they cannot decide (hex, inf, nan,
  # directed rounding, exact subnormals, ...). The test counts the calls that reach libc through the
  # STRICT_STOX_COUNT_LIBC seam, and checks exact halfway points, the text Apple's strtod aborts on, and
  # random texts against libc.
  add_executable(strict_stox_slow_test test/slow_test.cc)
  target_include_directories(strict_stox_slow_test PRIVATE test)
  target_compile_definitions(strict_stox_slow_test PRIVATE STRICT_STOX_COUNT_LIBC=1)
  target_link_libraries(strict_stox_slow_test PRIVATE strict_stox)
  add_test(NAME strict_stox_slow COMMAND strict_stox_slow_test)

  # The locale guard. The mock build overrides the decimal point the header sees (no installed locale
  # needed); the real build runs where a comma locale is installed (STRICT_STOX_TEST_LOCALE names one) and
  # says so when it skips. The C_LOCALE build checks the documented opt-out.
  add_executable(strict_stox_locale_mock_test test/locale_test.cc)
  target_compile_definitions(strict_stox_locale_mock_test PRIVATE LOCALE_TEST_MOCK=1)
  target_link_libraries(strict_stox_locale_mock_test PRIVATE strict_stox)
  add_test(NAME strict_stox_locale_mock COMMAND strict_stox_locale_mock_test)
  add_executable(strict_stox_locale_c_test test/locale_test.cc)
  target_compile_definitions(strict_stox_locale_c_test PRIVATE LOCALE_TEST_MOCK=1 STRICT_STOX_C_LOCALE=1)
  target_link_libraries(strict_stox_locale_c_test PRIVATE strict_stox)
  add_test(NAME strict_stox_locale_c COMMAND strict_stox_locale_c_test)
  add_executable(strict_stox_locale_test test/locale_test.cc)
  target_link_libraries(strict_stox_locale_test PRIVATE strict_stox)
  if(Threads_FOUND)
    target_link_libraries(strict_stox_locale_test PRIVATE Threads::Threads)
  endif()
  add_test(NAME strict_stox_locale COMMAND strict_stox_locale_test)

  # The 5^q table in strict_stox.hh is generated: it must match tools/gen_pow5.py.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME strict_stox_pow5_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_pow5.py --check)
  endif()

  # The independent harness (tools/verify.cc). Its long sweeps are run by hand (tools/verify_chain.sh,
  # .github/workflows/verify.yml); with the option on, ctest runs its hard cases and the short syntax
  # strings, which keeps the program building and its checks working.
  option(STRICT_STOX_BUILD_VERIFY "Build tools/verify.cc and two quick runs of it" OFF)
  if(STRICT_STOX_BUILD_VERIFY)
    add_executable(strict_stox_verify tools/verify.cc)
    target_compile_definitions(strict_stox_verify PRIVATE STRICT_STOX_COUNT_LIBC=1)
    target_compile_options(strict_stox_verify PRIVATE -Wno-ignored-attributes)
    target_link_libraries(strict_stox_verify PRIVATE strict_stox)
    if(Threads_FOUND)
      target_link_libraries(strict_stox_verify PRIVATE Threads::Threads)
    endif()
    add_test(NAME strict_stox_verify_hard COMMAND strict_stox_verify hard -t 2)
    add_test(NAME strict_stox_verify_syntax COMMAND strict_stox_verify syntax -n 4 -t 2)
  endif()

  # Benchmark (not run by ctest). Comparators (libc strto*, std::from_chars, fast_float, and
  # abseil when installed) are only linked by this target. fast_float is fetched at the tag
  # below unless -DSTRICT_STOX_FAST_FLOAT_DIR=<dir containing fast_float/fast_float.h> names one.
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
    set(STRICT_STOX_FAST_FLOAT_DIR "" CACHE PATH "include directory holding fast_float/fast_float.h (fetched when empty)")
    if(STRICT_STOX_FAST_FLOAT_DIR STREQUAL "")
      include(FetchContent)
      FetchContent_Declare(
        fast_float
        GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
        GIT_TAG        v8.3.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  none  # header-only: populate, do not add its tests
      )
      FetchContent_MakeAvailable(fast_float)
      set(STRICT_STOX_FAST_FLOAT_DIR "${fast_float_SOURCE_DIR}/include")
    endif()
    target_compile_definitions(strict_stox_bench PRIVATE HAVE_FAST_FLOAT=1)
    target_include_directories(strict_stox_bench PRIVATE "${STRICT_STOX_FAST_FLOAT_DIR}")
    # Floating-point std::from_chars needs a newer standard library than some platforms ship (libc++
    # before macOS 26 marks it unavailable); the column is dropped where it does not compile.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-std=c++20")
    check_cxx_source_compiles("#include <charconv>
      int main() { double d = 0; const char* s = \"1\"; std::from_chars(s, s + 1, d); }" STRICT_STOX_HAVE_FP_FROM_CHARS)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT STRICT_STOX_HAVE_FP_FROM_CHARS)
      target_compile_definitions(strict_stox_bench PRIVATE STRICT_STOX_BENCH_NO_FC=1)
    endif()
    # Results only, no timing: libc strto* must read every input the same as strict-stox.
    add_test(NAME strict_stox_bench_check COMMAND strict_stox_bench --check)
  endif()

  # A runnable tour of the library (not a test): build & run ./build/strict_stox_demo
  add_executable(strict_stox_demo examples/demo.cc)
  target_link_libraries(strict_stox_demo PRIVATE strict_stox)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(BUILD_FUZZING "Build sanitizer-backed libFuzzer target" OFF)
  if(BUILD_FUZZING)
    add_subdirectory(fuzz)
  endif()
endif()
