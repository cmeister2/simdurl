# Keep compiler and runtime changes in a new Bencher testbed/harness version.
FROM gcc:14.2.0-bookworm@sha256:b99b86a28812b1e6453a231a947dc43d76fe192788a12f344a9b568bf9f5d24c AS build
WORKDIR /src
COPY include/ include/
COPY benchmarks/bench.c benchmarks/bench_validate.c benchmarks/bencher/backend_info.c ./
ARG SOURCE_SHA
RUN mkdir /out && \
    for family in bench bench_validate; do \
      gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -Iinclude \
        "$family.c" -o "/out/simdurl_$family" && \
      gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -DSIMDURL_DISABLE_SIMD=1 -Iinclude \
        "$family.c" -o "/out/simdurl_${family}_scalar" || exit 1; \
    done && \
    gcc -std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY -Iinclude backend_info.c -o /out/backend_info && \
    printf '%s\n' "$SOURCE_SHA" > /out/source-sha && \
    printf '%s\n' '-std=c99 -O3 -DNDEBUG -DSIMDURL_HEADER_ONLY; scalar: -DSIMDURL_DISABLE_SIMD=1; no native tuning' > /out/compiler-flags

FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c
WORKDIR /opt/simdurl
COPY --from=build /out/ benchmarks/
COPY scripts/benchmark.py benchmarks/bencher/run.py ./
ENV PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["python3", "/opt/simdurl/run.py"]
