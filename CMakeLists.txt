cmake_minimum_required(VERSION 3.20)

project(simdurl VERSION 0.0.0 DESCRIPTION "SIMD URL encoding and decoding" LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(SIMDURL_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(SIMDURL_IS_TOP_LEVEL ON)
endif()
option(SIMDURL_BUILD_TESTS "Build simdurl tests" ${SIMDURL_IS_TOP_LEVEL})
option(SIMDURL_BUILD_BENCHMARKS "Build simdurl benchmarks" OFF)
option(SIMDURL_DISABLE_SIMD "Use only the portable scalar implementation" OFF)
option(SIMDURL_NATIVE "Tune for the build machine (binaries may not run on other CPUs)" OFF)
option(SIMDURL_ENABLE_IPO "Enable interprocedural optimization for the compiled library" OFF)

add_library(simdurl src/simdurl.c)
add_library(simdurl::simdurl ALIAS simdurl)
set_target_properties(simdurl PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  C_VISIBILITY_PRESET hidden)
target_include_directories(simdurl PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
get_target_property(SIMDURL_LIBRARY_TYPE simdurl TYPE)
if(SIMDURL_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(simdurl PRIVATE SIMDURL_BUILD PUBLIC SIMDURL_SHARED)
endif()

add_library(simdurl_header_only INTERFACE)
add_library(simdurl::header_only ALIAS simdurl_header_only)
set_target_properties(simdurl_header_only PROPERTIES EXPORT_NAME header_only)
target_include_directories(simdurl_header_only INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(simdurl_header_only INTERFACE SIMDURL_HEADER_ONLY)
# A C++-only consumer has no C compiler features for CMake to inspect.
target_compile_features(simdurl_header_only INTERFACE "$<$<BOOL:$<C_COMPILER_ID>>:c_std_99>")

if(SIMDURL_DISABLE_SIMD)
  target_compile_definitions(simdurl PRIVATE SIMDURL_DISABLE_SIMD)
  target_compile_definitions(simdurl_header_only INTERFACE SIMDURL_DISABLE_SIMD)
endif()

if(SIMDURL_NATIVE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR "SIMDURL_NATIVE requires GCC or Clang; set your compiler's CPU flags explicitly instead")
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|arm)")
    set(SIMDURL_NATIVE_FLAG "-mcpu=native")
  else()
    set(SIMDURL_NATIVE_FLAG "-march=native")
  endif()
  include(CheckCCompilerFlag)
  check_c_compiler_flag("${SIMDURL_NATIVE_FLAG}" SIMDURL_HAS_NATIVE_FLAG)
  if(NOT SIMDURL_HAS_NATIVE_FLAG)
    message(FATAL_ERROR "The compiler does not support ${SIMDURL_NATIVE_FLAG}")
  endif()
  target_compile_options(simdurl PRIVATE "${SIMDURL_NATIVE_FLAG}")
  # Installed header-only consumers select their own CPU flags.
  target_compile_options(simdurl_header_only INTERFACE "$<BUILD_INTERFACE:${SIMDURL_NATIVE_FLAG}>")
endif()

if(SIMDURL_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT SIMDURL_HAS_IPO OUTPUT SIMDURL_IPO_ERROR LANGUAGES C)
  if(NOT SIMDURL_HAS_IPO)
    message(FATAL_ERROR "IPO is unavailable: ${SIMDURL_IPO_ERROR}")
  endif()
  set_property(TARGET simdurl PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

install(TARGETS simdurl simdurl_header_only EXPORT simdurlTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/simdurl")

set(SIMDURL_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/simdurl")
configure_package_config_file(cmake/simdurlConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/simdurlConfig.cmake"
  INSTALL_DESTINATION "${SIMDURL_CMAKE_INSTALL_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/simdurlConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(EXPORT simdurlTargets FILE simdurlTargets.cmake NAMESPACE simdurl::
  DESTINATION "${SIMDURL_CMAKE_INSTALL_DIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/simdurlConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/simdurlConfigVersion.cmake"
  DESTINATION "${SIMDURL_CMAKE_INSTALL_DIR}")

if(SIMDURL_BUILD_TESTS)
  enable_language(CXX)
  enable_testing()
  add_subdirectory(tests)
  add_test(NAME install_consumer COMMAND "${CMAKE_COMMAND}"
    "-DSIMDURL_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DSIMDURL_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DSIMDURL_CONFIG=$<CONFIG>"
    "-DSIMDURL_GENERATOR=${CMAKE_GENERATOR}"
    "-DSIMDURL_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DSIMDURL_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DSIMDURL_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DSIMDURL_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DSIMDURL_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-install.cmake")
endif()
if(SIMDURL_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
