FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY minivectordb/CMakeLists.txt ./CMakeLists.txt
COPY minivectordb/src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04
COPY --from=build /src/build/mvdb /usr/local/bin/mvdb
RUN mkdir -p /data
WORKDIR /
ENTRYPOINT ["/usr/local/bin/mvdb"]
