FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM debian:bookworm-slim
RUN useradd -m -u 10001 dinodb
COPY --from=build /src/build/dinodb /usr/local/bin/dinodb
USER dinodb
WORKDIR /data
EXPOSE 7379
ENV DINODB_PORT=7379 DINODB_BIND=0.0.0.0 DINODB_RDB_PATH=/data/dinodb.rdb
ENTRYPOINT ["/usr/local/bin/dinodb"]
