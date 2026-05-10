# LLM Gateway — 多阶段构建
#   Stage 1: 构建依赖 (Drogon, llama.cpp, sol2)
#   Stage 2: 编译网关
#   Stage 3: 运行时镜像

# ─── Stage 1: 依赖构建 ────────────────────────────────────────────
FROM ubuntu:22.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libjsoncpp-dev libcurl4-openssl-dev libssl-dev \
    liblua5.3-dev libboost-all-dev nlohmann-json3-dev \
    pkg-config zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# sol2 (header-only, MIT license)
RUN git clone --depth 1 --branch v3.3.0 \
    https://github.com/ThePhD/sol2.git /opt/sol2

# Drogon (MIT license)
RUN git clone --depth 1 --branch v1.9.10 \
    https://github.com/drogonframework/drogon.git /opt/drogon && \
    cd /opt/drogon && git submodule update --init && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          .. && \
    make -j$(nproc) && make install && \
    rm -rf /opt/drogon/build

# llama.cpp (MIT license) — 可选，用于本地推理
RUN git clone --depth 1 --branch b4900 \
    https://github.com/ggerganov/llama.cpp.git /opt/llama.cpp && \
    cd /opt/llama.cpp && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_SHARED_LIBS=ON \
          -DLLAMA_CURL=OFF \
          .. && \
    make -j$(nproc) llama && \
    cp libllama.so /usr/local/lib/ && \
    ldconfig && \
    rm -rf /opt/llama.cpp/build

# ─── Stage 2: 编译网关 ─────────────────────────────────────────────
FROM deps AS builder

WORKDIR /src
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_STANDARD=20 \
          .. && \
    make -j$(nproc)

# ─── Stage 3: 运行时 ────────────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libjsoncpp25 libcurl4 libssl3 liblua5.3-0 \
    ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libdrogon.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libtrantor.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libllama.so* /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/build/llm_gateway /usr/local/bin/llm_gateway
COPY conf/ /etc/llm_gateway/conf/
COPY plugins/ /etc/llm_gateway/plugins/
COPY logs/ /var/log/llm_gateway/

RUN mkdir -p /var/log/llm_gateway /var/lib/llm_gateway/models

ENV LD_LIBRARY_PATH=/usr/local/lib

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/llm_gateway", "/etc/llm_gateway/conf/gateway.json"]
