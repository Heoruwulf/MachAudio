# MachAudio Protocol Specification (v1)

This document defines the MachAudio binary protocol used for ultra-low latency audio processing over Unix Domain Sockets (UDS) and TCP. It is designed for zero-allocation performance, deterministic latency, and easy integration with modern languages (Go, Python, Node.js, Rust, etc.).

## 1. General Principles

- **Endianness:** All integer and floating-point fields are in **Network Byte Order (Big-Endian)**.
- **Natural Alignment:** All message structures are designed to be naturally aligned on 4-byte or 8-byte boundaries.
- **Float Encoding:** Single-precision floats (IEEE 754) are serialized by copying their bits into a 32-bit integer and applying `htonl`.
- **Zero-Copy Intent:** Clients should aim to construct buffers in-place to minimize syscalls.

## 2. Message Header (`AudioMsgHeader`)

Every message, whether from client or server, begins with a fixed 16-byte header.

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `magic` | Constant `0x4D414348` ('MACH') |
| 4 | `uint16` | `version` | Constant `0x0001` |
| 6 | `uint16` | `command` | The `AudioCommand` ID (see below) |
| 8 | `uint32` | `sequence_id` | Client-provided ID for async correlation |
| 12 | `uint32` | `payload_len` | Length of the following payload (excluding header) |

---

## 3. Command Reference

### `CMD_START` (0x01)

**Direction:** Client → Server  
Initializes a processing session with specific codec and sample rate parameters.

**Payload (16 bytes):**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint8` | `in_pt` | Input Payload Type (0=PCMU, 8=PCMA, 96=L16, 111=Opus) |
| 1 | `uint8` | `in_ch` | Input Channels (1=Mono, 2=Stereo) |
| 2 | `uint16` | `flags` | Session Feature Flags (Bit 0 / `0x0001` = VAD enabled) |
| 4 | `uint32` | `in_rate` | Input Sample Rate (e.g., 8000, 16000, 48000) |
| 8 | `uint8` | `in_end` | Input Endianness (0=None, 1=LE, 2=BE) |
| 9 | `uint8` | `out_pt` | Output Payload Type |
| 10 | `uint8` | `out_ch` | Output Channels |
| 11 | `uint8` | `out_end` | Output Endianness |
| 12 | `uint32` | `out_rate` | Output Sample Rate |

---

### `CMD_INPUT` (0x02)

**Direction:** Client → Server  
Sends raw audio data for processing. Supports multiple sequential buffers.

**Payload Structure:**

1. **Container Header (8 bytes):**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `num_buffers` | Number of buffers in this message |
| 4 | `uint32` | `reserved` | Padding for 8-byte alignment |

2. **Sequential Buffers (Repeat `num_buffers` times):**
For each buffer, provide a header followed by the raw audio data.

**Buffer Header (8 bytes):**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `length` | Byte length of raw data following this header |
| 4 | `float` | `volume` | IEEE 754 float (0.0 to 1.0) |

- **Raw Data:** `length` bytes of raw audio.
- **Padding:** If `length` is not a multiple of 4, you **MUST** pad the data with null bytes to reach the next 4-byte boundary before starting the next buffer header.


---

### `CMD_OUTPUT` (0x03)

**Direction:** Server → Client  
Response to `CMD_INPUT` containing the transcoded/processed audio.

**Payload:**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint64` | `duration_ns` | Time spent in processing pipeline (nanoseconds) |
| 8 | `float` | `vad_prob` | VAD speech probability (`-1.0f` if disabled, `0.0f` to `1.0f` if enabled) |
| 12 | `uint8[4]` | `reserved` | Padding to maintain 8-byte alignment for flexible data array |
| 16 | `uint8[]` | `data` | Raw processed audio data (size = `payload_len - 16`) |

---

### `CMD_STOP` (0x04)

**Direction:** Client → Server  
Closes the processing session and frees worker resources.
**Payload:** None (`payload_len` = 0).

---

### `CMD_ERROR` (0x05)

**Direction:** Server → Client  
Sent when a request fails or the protocol is violated.

**Payload (4 bytes):**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `error_code` | Error code integer (see Error Codes section) |

---

### `CMD_PING` (0x06) / `CMD_PONG` (0x07)

**Direction:** Bi-directional  
Keep-alive mechanism. Server responds to `CMD_PING` with `CMD_PONG`.
**Payload:** None.

---

### `CMD_DISCOVER` (0x08) / `CMD_DISCOVER_REPLY` (0x09)

**Direction:** Client ↔ Server  
Used to determine the cluster topology.

**`CMD_DISCOVER` Payload:** None.
**`CMD_DISCOVER_REPLY` Payload (8 bytes):**

| Offset | Type | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `num_workers` | Total number of running worker processes |
| 4 | `uint32` | `reserved` | Padding |

---

## 4. Error Codes

| Code | Name | Description |
| :--- | :--- | :--- |
| 1 | `ERR_INVALID_MAGIC` | Header magic field is incorrect |
| 2 | `ERR_UNSUPPORTED_VERSION` | Protocol version is not supported |
| 3 | `ERR_INVALID_COMMAND` | Command ID is unknown |
| 4 | `ERR_INVALID_PAYLOAD` | Payload size or content is invalid |
| 5 | `ERR_PROCESSING_FAILED` | Transcoding or resampling engine failure |
| 6 | `ERR_INTERNAL_ERROR` | Unexpected server-side fault |

---

## 5. Implementation Guidance

When creating client code:

1. **Packing:** Use standard library "struct" or "buffer" modules (e.g., `struct.pack` in Python, `Buffer` in Node.js, `binary.Write` in Go).
2. **Streaming:** MachAudio is a streaming service. Maintain a persistent UDS or TCP connection. Do not open/close for every message.
3. **Pacing:** Send `CMD_INPUT` chunks corresponding to the real-time `ptime` (packet time) of the audio (usually 20ms).
4. **Discovery & Load Balancing:**
   - MachAudio spawns workers sequentially.
   - For UDS: `path.0.sock`, `path.1.sock`, etc.
   - For TCP: `port`, `port + 1`, etc.
   - **Logic:**
     1. Initial Connection: Connect to worker index 0 (e.g., `machaudio.0.sock` or base port `8000`).
     2. Discovery: Send `CMD_DISCOVER` to retrieve the total `num_workers` available in the cluster.
     3. Load Balancing: For every new audio session (e.g., a new stream or call), the client should select a worker using a **round-robin** strategy to spread work evenly across the pool.
     4. Persistence: Establish a persistent connection to the selected worker's specific socket/port for the duration of that session.
5. **Voice Activity Detection (VAD) Integration:**
   - **Activation:** Set the Least Significant Bit (`0x0001`) of the `flags` field in the `CMD_START` payload to enable real-time speech detection.
   - **Inference Pipeline:** When enabled, the server runs a zero-allocation Micro-GRU neural network directly on 16kHz resampled/mono PCM buffers.
   - **Probability Extraction:** In each `CMD_OUTPUT` frame, extract the network-ordered float `vad_prob`. Values range from `0.0f` to `1.0f` (representing voice activity confidence), while `-1.0f` is returned if VAD is disabled or unavailable.
   - **Performance:** With VAD enabled (with encode, resample, and mix) on a 20ms packet time (`ptime 20`), the average processing latency remains under **0.364 ms** (with a **0.411 ms** P95 latency), ensuring immediate, real-time agent turn-taking response times.
