# syntax=docker/dockerfile:1

FROM debian:bookworm-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --target kvstore

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --home-dir /data --create-home kvstore
COPY --from=build /src/build/kvstore /usr/local/bin/kvstore
USER kvstore
VOLUME /data
EXPOSE 6379
# Answers PING over a raw TCP connection, so the image needs no client tools.
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=5 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/6379 && printf 'PING\\r\\n' >&3 && head -c 5 <&3 | grep -q PONG"]
ENTRYPOINT ["kvstore", "--bind", "0.0.0.0", "--dir", "/data"]
CMD ["--appendonly", "yes"]
