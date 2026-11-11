FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-13 cmake make git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY bench ./bench
COPY examples ./examples
ENV CC=gcc-13 CXX=g++-13
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKVBM_WERROR=ON \
    && cmake --build build -j"$(nproc)" \
    && ./build/kvbm_tests --gtest_brief=1

FROM ubuntu:24.04
RUN useradd --create-home --uid 10001 kv
COPY --from=build /src/build/kvbm_simulate /usr/local/bin/kvbm_simulate
COPY --from=build /src/build/kvbm_bench /usr/local/bin/kvbm_bench
USER kv
ENTRYPOINT ["kvbm_simulate"]
CMD ["--requests", "500"]
