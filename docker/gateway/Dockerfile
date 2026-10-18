# syntax=docker/dockerfile:1
#
# Edge gateway image. Two stages: the runtime stage contains the stripped Release executable
# and its shared-library dependencies only, with no compiler or build tools.
# Build context: the repository root (see the whitelist in /.dockerignore).

FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ libmbedtls-dev ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY gateway ./gateway
# The build directory is a BuildKit cache, so rebuilds reuse fetched and compiled dependencies.
RUN --mount=type=cache,target=/build \
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGATEWAY_BUILD_TESTS=OFF \
    && cmake --build /build --target gateway \
    && install -D -m 0755 -s /build/gateway/gateway /out/ieg-gateway

FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
# Runtime libraries of the official open62541 amalgamation's mbedTLS dependency.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libmbedcrypto7t64 libmbedtls14t64 libmbedx509-1t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10002 --no-create-home --shell /usr/sbin/nologin gateway
COPY --from=build /out/ieg-gateway /usr/local/bin/ieg-gateway
# Default configuration = the Compose/demo configuration (service names plant-simulator and
# mosquitto). Standalone deployments must supply a configuration with their real OPC UA and
# MQTT endpoints, e.g. by mounting it over this path or passing another path as the argument.
COPY configs/gateway.example.yaml /etc/ieg/gateway.yaml
USER gateway
# Exec form: the gateway is PID 1 and receives SIGTERM directly (it blocks SIGINT/SIGTERM and
# collects them with sigwait, so the signal is not dropped by the PID 1 rules).
ENTRYPOINT ["/usr/local/bin/ieg-gateway"]
CMD ["/etc/ieg/gateway.yaml"]
