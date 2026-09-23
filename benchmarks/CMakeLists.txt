add_executable(simdurl_bench bench.c)
target_link_libraries(simdurl_bench PRIVATE simdurl::header_only)
add_executable(simdurl_bench_scalar bench.c)
target_link_libraries(simdurl_bench_scalar PRIVATE simdurl::header_only)
target_compile_definitions(simdurl_bench_scalar PRIVATE SIMDURL_DISABLE_SIMD=1)
add_executable(simdurl_bench_validate bench_validate.c)
target_link_libraries(simdurl_bench_validate PRIVATE simdurl::header_only)
add_executable(simdurl_bench_validate_scalar bench_validate.c)
target_link_libraries(simdurl_bench_validate_scalar PRIVATE simdurl::header_only)
target_compile_definitions(simdurl_bench_validate_scalar PRIVATE SIMDURL_DISABLE_SIMD=1)
add_executable(simdurl_bench_formscan bench_formscan.c)
target_link_libraries(simdurl_bench_formscan PRIVATE simdurl::header_only)
add_executable(simdurl_bench_formscan_portable bench_formscan.c)
target_link_libraries(simdurl_bench_formscan_portable PRIVATE simdurl::header_only)
target_compile_definitions(simdurl_bench_formscan_portable PRIVATE SIMDURL_DISABLE_SIMD=1)
add_executable(simdurl_bench_formscan_compiled bench_formscan.c)
target_link_libraries(simdurl_bench_formscan_compiled PRIVATE simdurl::simdurl)
target_compile_definitions(simdurl_bench_formscan_compiled PRIVATE SIMDURL_BENCH_COMPILED=1)
if((WIN32 OR CYGWIN) AND SIMDURL_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  add_custom_command(TARGET simdurl_bench_formscan_compiled POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:simdurl>" "$<TARGET_FILE_DIR:simdurl_bench_formscan_compiled>"
    VERBATIM)
endif()
foreach(target IN ITEMS simdurl_bench simdurl_bench_scalar
                        simdurl_bench_validate simdurl_bench_validate_scalar
                        simdurl_bench_formscan simdurl_bench_formscan_portable
                        simdurl_bench_formscan_compiled)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
endforeach()
