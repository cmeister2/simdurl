# Each fuzzer compiles the header-only implementation so all library code is
# instrumented, regardless of flags used for the ordinary compiled library.
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
  message(FATAL_ERROR "SIMDURL_BUILD_FUZZERS requires Clang with the libFuzzer runtime")
endif()

foreach(operation IN ITEMS encode decode scanner)
  set(target simdurl_fuzz_${operation})
  add_executable(${target} ${operation}.c)
  target_link_libraries(${target} PRIVATE simdurl::header_only)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  add_test(NAME ${target}_corpus COMMAND ${target}
    -runs=0 -max_len=4104 "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${operation}")
  set_tests_properties(${target}_corpus PROPERTIES LABELS "fuzz;corpus" TIMEOUT 60)
endforeach()
