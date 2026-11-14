cmake_minimum_required(VERSION 3.14)
project(strict_stox_platform_repro LANGUAGES C CXX)
add_executable(libc_strtofp_repro libc_strtofp.c)
add_executable(strict_stox_strtofp_repro strict_strtofp.cc)
target_compile_features(strict_stox_strtofp_repro PRIVATE cxx_std_20)
target_include_directories(strict_stox_strtofp_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
enable_testing()
foreach(fixture original minimal)
  foreach(kind strtof strtod strtold)
    add_test(NAME platform_libc_${kind}_${fixture} COMMAND libc_strtofp_repro ${kind}
      "${CMAKE_CURRENT_SOURCE_DIR}/apple-strtofp-${fixture}.txt")
  endforeach()
  foreach(kind stof stod stold)
    add_test(NAME platform_strict_${kind}_${fixture} COMMAND strict_stox_strtofp_repro ${kind}
      "${CMAKE_CURRENT_SOURCE_DIR}/apple-strtofp-${fixture}.txt")
    add_test(NAME platform_strict_${kind}_prefix_${fixture} COMMAND strict_stox_strtofp_repro ${kind}
      "${CMAKE_CURRENT_SOURCE_DIR}/apple-strtofp-${fixture}.txt" prefix)
  endforeach()
endforeach()
