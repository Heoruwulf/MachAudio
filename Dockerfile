# ==========================================
# Stage 1: Builder
# ==========================================
FROM alpine:3.22.4 AS builder

# Install build dependencies
# Note: build-base includes gcc, g++, and make
RUN apk add --no-cache \
    build-base \
    cmake \
    pkgconf \
    git \
    liburing-dev \
    opus-dev

# Set working directory
WORKDIR /src

# Copy the entire project into the builder
COPY . .

# Build the project
# - Release mode optimizes the binary.
# - AVX2 is already enforced in the project's CMakeLists.txt (COMMON_FLAGS).
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) machaudio

# ==========================================
# Stage 2: Runtime
# ==========================================
FROM alpine:3.22.4

# Install runtime dependencies ONLY
RUN apk add --no-cache \
    liburing \
    opus

# Security best practice: Create a non-root user to run the daemon
RUN addgroup -S machaudio && adduser -S machaudio -G machaudio

# Copy the compiled binary and entrypoint script from the builder stage
COPY --from=builder /src/build/bin/machaudio /usr/local/bin/machaudio
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Drop privileges
USER machaudio

# Expose the default base port (TCP)
EXPOSE 8000

# ENTRYPOINT defines the runtime wrapper script
ENTRYPOINT ["docker-entrypoint.sh"]

# CMD provides the default networking arguments.
# The entrypoint will dynamically append `--workers` based on CPU core count.
# Example override: docker run -p 8080-8083:8080-8083 machaudio --host 0.0.0.0 --port 8080 --workers 4
CMD ["--host", "0.0.0.0", "--port", "8000"]
