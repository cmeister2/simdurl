cmake_minimum_required(VERSION 3.20)

function(simdurl_run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}")
  endif()
endfunction()

set(test_root "${SIMDURL_BINARY_DIR}/install-test")
set(original_prefix "${test_root}/original")
set(relocated_prefix "${test_root}/relocated")
file(REMOVE_RECURSE "${test_root}")
simdurl_run("${CMAKE_COMMAND}" --install "${SIMDURL_BINARY_DIR}"
  --config "${SIMDURL_CONFIG}" --prefix "${original_prefix}")
file(RENAME "${original_prefix}" "${relocated_prefix}")
if(CYGWIN)
  set(ENV{PATH} "${relocated_prefix}/${SIMDURL_INSTALL_BINDIR}:$ENV{PATH}")
elseif(WIN32)
  set(ENV{PATH} "${relocated_prefix}/${SIMDURL_INSTALL_BINDIR};$ENV{PATH}")
endif()
set(generator_args -G "${SIMDURL_GENERATOR}")
if(SIMDURL_GENERATOR_PLATFORM)
  list(APPEND generator_args -A "${SIMDURL_GENERATOR_PLATFORM}")
endif()
if(SIMDURL_GENERATOR_TOOLSET)
  list(APPEND generator_args -T "${SIMDURL_GENERATOR_TOOLSET}")
endif()
foreach(consumer_cxx IN ITEMS OFF ON)
  if(consumer_cxx)
    set(compiler_arg "-DCMAKE_CXX_COMPILER=${SIMDURL_CXX_COMPILER}")
  else()
    set(compiler_arg "-DCMAKE_C_COMPILER=${SIMDURL_C_COMPILER}")
  endif()
  foreach(header_only IN ITEMS OFF ON)
    set(consumer_build "${test_root}/consumer-cxx-${consumer_cxx}-header-${header_only}")
    simdurl_run("${CMAKE_COMMAND}"
      -S "${SIMDURL_SOURCE_DIR}/tests/consumer" -B "${consumer_build}"
      ${generator_args} "${compiler_arg}"
      "-DCMAKE_PREFIX_PATH=${relocated_prefix}"
      "-DCMAKE_BUILD_TYPE=${SIMDURL_CONFIG}"
      "-DSIMDURL_CONSUMER_CXX=${consumer_cxx}"
      "-DSIMDURL_USE_HEADER_ONLY=${header_only}")
    simdurl_run("${CMAKE_COMMAND}" --build "${consumer_build}" --config "${SIMDURL_CONFIG}")
    simdurl_run("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}"
      -C "${SIMDURL_CONFIG}" --output-on-failure)
  endforeach()
endforeach()
