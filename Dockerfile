FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    make \
    pkg-config \
    libboost-system-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libsqlite3-dev \
    libhiredis-dev \
    libjsoncpp-dev \
    nlohmann-json3-dev \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DBUILD_TESTING=OFF && \
    cmake --build build --target \
      mindbridge_platform \
      mindbridge_gateway \
      mindbridge_orchestrator \
      mindbridge_counselor \
      mindbridge_evaluator \
      mindbridge_benchmark \
      -j2

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    python3 \
    libboost-system1.74.0 \
    libcurl4 \
    libssl3 \
    libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/mindbridge_harness/mindbridge_platform /app/
COPY --from=builder /src/build/mindbridge_harness/mindbridge_gateway /app/
COPY --from=builder /src/build/mindbridge_harness/mindbridge_orchestrator /app/
COPY --from=builder /src/build/mindbridge_harness/mindbridge_counselor /app/
COPY --from=builder /src/build/mindbridge_harness/mindbridge_evaluator /app/
COPY --from=builder /src/build/mindbridge_harness/mindbridge_benchmark /app/
COPY start-service.sh /app/
COPY scripts/serve_demo_frontend.py /app/scripts/
COPY frontend/demo /app/frontend/demo

RUN chmod +x /app/start-service.sh && mkdir -p /app/.mindbridge/platform /app/.mindbridge/runs /app/.mindbridge/state

ENTRYPOINT ["/app/start-service.sh"]
