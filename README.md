# MachAudio

![C11](https://img.shields.io/badge/Standard-C11-blue.svg)
![Build](https://img.shields.io/badge/Build-CMake-success.svg)
![Tests](https://img.shields.io/badge/Tests-Unity-green.svg)
![License](https://img.shields.io/badge/License-AGPLv3-red.svg)

High-throughput C11 audio daemon for VoIP AI agents. Features a pre-forking reactor model, zero-copy IPC, and AVX2-optimized kernels for deterministic, ultra-low latency audio on Linux.

The service communicates over Unix Domain Sockets (UDS) and TCP/IP using a custom, naturally aligned binary protocol, ensuring minimal IPC overhead.

## 🚀 Key Features

* **Massive Concurrency:** A pre-forking multi-core worker architecture allows the server to scale linearly across CPU cores, dynamically tuning `RLIMIT_NOFILE` and `SOMAXCONN` to handle thousands of concurrent real-time audio streams.
* **Zero-Allocation Hot Path:** Employs request-scoped Arenas and session-scoped Pools. `malloc` and `free` are strictly forbidden during active audio processing to guarantee deterministic latency.
* **AVX2 / SIMD Optimized:** computationally intensive operations, such as G.711 Look-Up Table gathering and Polyphase FIR filter resampling, are heavily optimized using AVX2 intrinsics to maximize memory bandwidth and throughput.
* **Comprehensive Transcoding:**
  * **G.711 (PCMU / PCMA):** High-speed compression/expansion to and from 16-bit linear PCM.
  * **Opus:** Integration with `libopus` for high-quality, low-latency VoIP encoding and decoding.
  * **L16:** Raw 16-bit PCM with automatic endianness swapping based on host and protocol specifications.
* **Dynamic Resampling:** High-quality sample rate conversion (e.g., 8kHz to 48kHz) using SIMD-accelerated polyphase FIR filters.
* **Voice Activity Detection (VAD):** Integrated zero-allocation, real-time Voice Activity Detection using a Micro-GRU neural network architecture, accelerated with AVX2/FMA intrinsics. Returns real-time speech probability (0.0f to 1.0f) packaged directly in output frames. For architecture details, dataset sensitivities, and the VAD Spectrogram Dashboard, see the [Micro-GRU VAD Documentation](docs/micro-gru-vad.md).
* **Asynchronous I/O:** Event-driven architecture utilizing a dedicated, native `io_uring` proactor event loop per worker process.

## ⚡ Performance & Capacity Projections

MachAudio is designed for ultra-low latency. Below are empirical benchmark results running a single worker process on a modern x86_64 CPU utilizing the native `io_uring` proactor event loop (AVX2 enabled, `SCHED_FIFO` real-time priority 90, `performance` CPU governor locked, worker thread pinned to isolated core 8).

**Test Scenario: Real-Time Two-Stream Mixing, Transcoding & Voice Activity Detection (UDS Mix)**

* **Input Streams:** 2 parallel input streams of G.711 mu-law (8kHz Mono, ptime 20ms)
* **Output Stream:** Mixed, resampled, and transcoded to raw L16 (16kHz Mono, Little-Endian)
* **Voice Activity Detection (VAD):** Enabled on mixed stream (Micro-GRU neural network running inline)
* **Load Duration:** 120 seconds of real-time audio streams (5,980 frames processed)
* **Average Latency:** **0.043 ms** (43 microseconds) per buffer
* **P95 Latency:** **0.047 ms** (47 microseconds)
* **Min Latency:** **0.039 ms** (39 microseconds)
* **Max Latency:** **0.072 ms** (72 microseconds)

### Capacity Projections (Per Core)

Because MachAudio guarantees sub-millisecond processing via zero-copy `io_uring` queues, a zero-allocation hot path, and AVX2 vectorization, a single worker thread pinned to a single physical core can sustain massive concurrent density.

Based on an average latency of **~0.043ms** to process a standard **20ms** audio frame (including decoding both streams, mixing, resampling, neural-network VAD, and output delivery):

* **~450 concurrent real-time streams** per physical core.

Linear scaling is achieved via the pre-forking architecture. A standard 8-core instance running 8 workers can safely handle upwards of **3,600 concurrent real-time streams** (a massive capacity increase over the legacy `libuv`/`epoll` implementation), providing extreme throughput for professional VoIP AI agents without garbage collection pauses or context-switching jitter.

## 🏗 Architecture

1. **Transport Layer:** Native `io_uring` proactor stream handling supports both Unix Domain Sockets (e.g., `/tmp/machaudio.N.sock`) and TCP/IP (e.g., port `8000 + N`), managing non-blocking reads and writes without system-call overhead. For details on container orchestration and auto-scaling, see the [Deployment Guide](docs/deploy.md) and [DevOps & Deployment Guide](docs/devops.md).
2. **Protocol Layer:** A custom, naturally aligned sequential TLV binary protocol. It parses fixed-size headers (`AudioMsgHeader`) and structured payloads including `CMD_START`, `CMD_INPUT` (supporting multiple sequential audio buffers), `CMD_DISCOVER`, `CMD_PING`, and explicit `CMD_ERROR` states. For detailed integration info, see the [Protocol Specification](docs/protocol.md).
3. **Processing Pipeline:**
    * **Decode:** Converts incoming streams (Opus, PCMU, PCMA) to a normalized L16 format within the Arena.
    * **Resample:** Applies a polyphase FIR filter if the input and output sample rates differ, writing to a new block in the Arena.
    * **Encode:** Compresses the normalized (and optionally resampled) L16 buffer back to the requested output format.

## 🛠 Building and Running

### Prerequisites

Ensure you have a C11 compatible compiler, CMake (3.15+), and the required development headers installed.

* **Debian/Ubuntu:** `sudo apt-get install cmake gcc clang clang-tools liburing-dev libuv1-dev libopus-dev`
* **Alpine:** `apk add cmake gcc clang clang-extra-tools musl-dev liburing-dev libuv-dev opus-dev`

### Build Instructions

The project uses a modern target-based CMake configuration.

```bash
mkdir -p build && cd build

# Configure (Debug mode enables ASan and UBSan)
export CC=clang
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build
make
```

### Continuous Integration & Testing

MachAudio is rigorously tested to prevent undefined behavior and memory leaks:

```bash
./scripts/ci.sh
```

This script executes a 3-phase validation:

1. **Static Analysis:** Uses `scan-build` to detect deep logical flaws.
2. **Fast Path:** Runs the Unity TDD suite compiled with ASan and UBSan.
3. **Parser Hardening:** Compiles the protocol parsers against `libFuzzer` and executes millions of coverage-guided mutations to prove resilience against malicious packet payloads.

#### Real-Time Optimization & System Tuning

To achieve the ultra-low and deterministic latency required for professional audio, MachAudio provides CLI flags for OS-level tuning. AVX2 SIMD instructions are always enabled by default in the Release build.

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make && cd ..
```

### Running the Service Daemon

You can start the daemon with standard privileges, or use the CLI flags to spawn multiple workers and isolate processes:

```bash
./build/bin/machaudio [options]
```

**Options:**

* `-w, --workers <N>`: Forks `N` independent worker processes (default: 1).
* `-c, --cpu-core <M>`: The starting (base) logical CPU core for thread pinning (default: 0).
* `-p, --rt-priority <N>`: Elevates the thread scheduling policy to `SCHED_FIFO` (real-time) with the specified priority (1-99).
* `-H, --host <IP>`: Activates TCP mode and binds workers to this host.
* `-P, --port <P>`: The starting TCP port (default: 8000).
* `-D, --uds-dir <dir>`: Directory for Unix Domain Sockets (default: `/tmp`).
* `-L, --fd-limit <N>`: Set the maximum open file descriptors limit (`RLIMIT_NOFILE`) for the daemon (default: 4096).

**How Worker Topology & Core Pinning Works:**

The supervisor process automatically spreads workers sequentially to maximize L1/L2 cache locality and prevent OS CPU migration. It uses a **base + offset** strategy.

If you specify `-w 4 -c 2`, the supervisor will fork 4 workers:

* **Worker 0:** Pinned to core **2** (`2 + 0`), listening on port **8000** (or `/tmp/machaudio.0.sock`).
* **Worker 1:** Pinned to core **3** (`2 + 1`), listening on port **8001** (or `/tmp/machaudio.1.sock`).
* **Worker 2:** Pinned to core **4** (`2 + 2`), listening on port **8002** (or `/tmp/machaudio.2.sock`).
* **Worker 3:** Pinned to core **5** (`2 + 3`), listening on port **8003** (or `/tmp/machaudio.3.sock`).

*Note: The client can automatically discover this topology by connecting to Worker 0 and sending a `CMD_DISCOVER` message.*

**Permissions:**
Using the `--rt-priority` flag requires elevated privileges. You must either run the server as `root` (`sudo`) or grant the binary the `CAP_SYS_NICE` capability:

```bash
sudo setcap 'cap_sys_nice=ep' ./build/bin/machaudio
./build/bin/machaudio --workers 4 --cpu-core 2 --rt-priority 85
```

**Host Tuning Script:**
For maximum latency reduction, it is recommended to disable CPU frequency scaling (C-states) on the target cores. A helper script is provided to set the scaling governor to `performance`:

```bash
sudo ./scripts/tune-host.sh 2
```

## 👨‍💻 Development Conventions

* **Strict C11:** The project adheres to ISO/IEC 9899:2011.
* **Zero-Warning Policy:** Builds fail on warnings (`-Wall -Wextra -Wpedantic -Werror`).

## 📄 License

MachAudio is released under the **GNU Affero General Public License v3 (AGPL-3.0)**.

This ensures that the software remains free and open source, even when used as a network service. Any modifications to this codebase must be made publicly available under the same license. For more details, see the [LICENSE](./LICENSE) file.
