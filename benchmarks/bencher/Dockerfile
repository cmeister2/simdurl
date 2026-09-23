# Keep compiler and runtime changes in a new Bencher testbed/harness version.
FROM gcc:14.3.0-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15 AS build
WORKDIR /src
COPY include/ include/
COPY benchmarks/bench.c benchmarks/bench_validate.c benchmarks/bench_formscan.c benchmarks/bencher/backend_info.c ./
COPY src/simdurl.c ./simdurl.c
ARG SOURCE_SHA
RUN mkdir /out && \
    for family in bench bench_validate bench_formscan; do \
      scalar_suffix=scalar; \
      if [ "$family" = bench_formscan ]; then scalar_suffix=portable; fi; \
      gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -Iinclude \
        "$family.c" -o "/out/simdurl_$family" && \
      gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -DSIMDURL_DISABLE_SIMD=1 -Iinclude \
        "$family.c" -o "/out/simdurl_${family}_${scalar_suffix}" || exit 1; \
    done && \
    gcc -std=c99 -O3 -DNDEBUG -Iinclude -c simdurl.c -o /tmp/simdurl.o && \
    gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_BENCH_COMPILED=1 -Iinclude \
      bench_formscan.c /tmp/simdurl.o -o /out/simdurl_bench_formscan_compiled && \
    gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -Iinclude backend_info.c -o /out/backend_info && \
    printf '%s\n' "$SOURCE_SHA" > /out/source-sha && \
    printf '%s\n' '-std=c99 -O3 -DNDEBUG; header-only: -DSIMDURL_HEADER_ONLY; scalar: -DSIMDURL_DISABLE_SIMD=1; compiled formscan: separate simdurl.c object and -DSIMDURL_BENCH_COMPILED=1 benchmark; no native tuning or LTO' > /out/compiler-flags

FROM python:3.14.7-slim-bookworm@sha256:82bc3c539b8813ada9d68c63b40158fa002f7f33de9bf3312a3dfdc0620dff56
WORKDIR /opt/simdurl
COPY --from=build /out/ benchmarks/
COPY scripts/benchmark.py benchmarks/bencher/run.py ./
ENV PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["python3", "/opt/simdurl/run.py"]
