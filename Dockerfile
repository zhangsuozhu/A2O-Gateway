FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config ca-certificates \
    libevent-dev libcurl4-openssl-dev libcjson-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build && cmake --build build -j

FROM debian:12-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libevent-2.1-7 libevent-pthreads-2.1-7 libcurl4 libcjson1 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/cc-oai-gateway /usr/local/bin/cc-oai-gateway
COPY web ./web
COPY config ./config
EXPOSE 8080
ENV GATEWAY_CONFIG=/app/config/gateway.json
CMD ["cc-oai-gateway", "/app/config/gateway.json"]
