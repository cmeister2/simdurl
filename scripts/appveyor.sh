#!/usr/bin/env bash
set -euo pipefail

phase=${1:?Specify build or test}
case "$phase" in
  build|test) ;;
  *) printf 'Unknown phase: %s\n' "$phase" >&2; exit 1 ;;
esac
cd "$(cygpath -u "${APPVEYOR_BUILD_FOLDER:?}")"

case "${SIMDURL_TOOLCHAIN:?}" in
  UCRT64) toolchain_bin=/ucrt64/bin; cc=gcc; cxx=g++; generator=Ninja ;;
  CLANG64) toolchain_bin=/clang64/bin; cc=clang; cxx=clang++; generator=Ninja ;;
  MINGW32) toolchain_bin=/mingw32/bin; cc=gcc; cxx=g++; generator=Ninja ;;
  CYGWIN64) toolchain_bin=/usr/bin; cc=gcc; cxx=g++; generator='Unix Makefiles' ;;
  *) printf 'Unknown toolchain: %s\n' "$SIMDURL_TOOLCHAIN" >&2; exit 1 ;;
esac
export PATH="$toolchain_bin:/usr/bin:/bin:$PATH"
cmake="$toolchain_bin/cmake.exe"
ctest="$toolchain_bin/ctest.exe"
cc="$toolchain_bin/$cc.exe"
cxx="$toolchain_bin/$cxx.exe"
if [[ "$SIMDURL_TOOLCHAIN" != CYGWIN64 ]]; then
  cmake=$(cygpath -u "${SIMDURL_CMAKE:?}")
  ctest=$(cygpath -u "${SIMDURL_CTEST:?}")
  ninja=$(cygpath -u "${SIMDURL_NINJA:?}")
  # Keep Ninja available to the separate installed-consumer CMake builds too.
  export PATH="$(dirname "$cmake"):$(dirname "$ninja"):$PATH"
  # Native Windows CMake expects native paths for explicit compiler locations.
  cc=$(cygpath -m "$cc")
  cxx=$(cygpath -m "$cxx")
fi

if [[ "$phase" == build ]]; then
  "$cmake" --version
  "$cc" --version
  "$cxx" --version
fi

for linkage in static shared; do
  build_dir="build/appveyor-${SIMDURL_TOOLCHAIN,,}-$linkage"
  shared=OFF
  if [[ "$linkage" == shared ]]; then
    shared=ON
  fi

  if [[ "$phase" == build ]]; then
    "$cmake" -S . -B "$build_dir" -G "$generator" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
      -DBUILD_SHARED_LIBS="$shared" \
      -DSIMDURL_BUILD_TESTS=ON -DSIMDURL_BUILD_BENCHMARKS=ON
    "$cmake" --build "$build_dir" --parallel 2
  else
    "$ctest" --test-dir "$build_dir" --output-on-failure --parallel 2 --timeout 300
    "$build_dir/benchmarks/simdurl_bench.exe" 10
    "$build_dir/benchmarks/simdurl_bench_scalar.exe" 10
  fi
done
