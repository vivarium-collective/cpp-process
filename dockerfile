FROM debian:12-slim

# Install build deps and runtime tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make pkg-config \
    nlohmann-json3-dev \
    netcat-openbsd \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy source
COPY CMakeLists.txt .
COPY src ./src
COPY config ./config

# Build
RUN cmake -S . -B build && cmake --build build --config Release

ENV PORT=11111
EXPOSE 11111

# Default config mount point: /config/config.json
CMD ["/app/build/vivarium_cpp_process"]
