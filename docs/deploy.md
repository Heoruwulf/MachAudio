# MachAudio Deployment & Orchestration Guide

This document provides DevOps and Site Reliability Engineers with the necessary context to deploy, scale, and orchestrate the MachAudio daemon in containerized environments (Docker, Kubernetes, ECS, etc.).

## 1. The Concurrency Model (Workers & Cores)

MachAudio is a C11 application utilizing a **pre-forking multi-core worker model**. To achieve ultra-low latency, it avoids multi-threading lock contention by forking entirely independent worker processes, each running its own `libuv` event loop.

### Auto-Scaling in Containers

The provided Docker image includes a dynamic `docker-entrypoint.sh` script. When the container starts, it calculates the number of available CPUs allocated to it via the OS `nproc` command.

By default, the entrypoint will automatically spawn **`nproc - 1` workers** (with a minimum of 1).

* *Example:* If you assign a Kubernetes Pod a CPU limit of `8.0`, the container will automatically spawn 7 workers.

You can explicitly override this by passing the `--workers <N>` or `-w <N>` flag to the container command.

### CPU Pinning

When running directly on a Linux host, MachAudio accepts a `--cpu-core <M>` argument to pin workers to specific logical cores using a "base + offset" strategy to preserve L1/L2 cache locality.

**Note for Container Environments:** CPU pinning is generally handled by the orchestrator (e.g., Kubernetes `Static` CPU Manager policy). If you are relying on Kubernetes for core affinity, you do **not** need to pass `--cpu-core` to the MachAudio binary.

## 2. Networking and Port Allocation

MachAudio is highly unique in how it handles networking across its workers. **It does not use a shared listening socket.**

### The Base Port Offset

When running in TCP mode, the supervisor process assigns each worker a specific, sequential port starting from the `--port` argument (default: `8000`).

If a container spawns 4 workers (either manually or via auto-scaling), they will bind internally to:

* Worker 0: `8000`
* Worker 1: `8001`
* Worker 2: `8002`
* Worker 3: `8003`

### The `EXPOSE` Misconception

The `Dockerfile` explicitly lists `EXPOSE 8000`. **Do not modify this to expose a massive port range.**
In Docker, `EXPOSE` is purely documentation indicating the "Base Port". Because MachAudio scales dynamically at runtime, hardcoding `EXPOSE 8000-8100` is anti-pattern and confusing.

## 3. Deployment Examples

### Docker (Standalone)

When running via standard Docker, you must publish the range of ports corresponding to the number of workers you expect.

**Example 1: Let the container auto-scale (assuming a 4-core host)**

```bash
# Binds ports 8000, 8001, and 8002 to the host
docker run -d --name machaudio -p 8000-8002:8000-8002 machaudio:latest
```

**Example 2: Explicitly override workers and base port**

```bash
# Forces 8 workers starting at port 9000
docker run -d --name machaudio -p 9000-9007:9000-9007 machaudio:latest --port 9000 --workers 8
```

### Kubernetes

To properly route traffic to MachAudio in Kubernetes, you should pair the deployment with a **Headless Service**.

Because clients must connect to specific worker ports, putting a standard LoadBalancer Service in front of MachAudio is an anti-pattern. Instead, clients should use the `CMD_DISCOVER` protocol message on the Base Port (8000) to find out how many workers are alive, and then establish direct persistent TCP connections to the sequential ports.

**Example Pod Spec:**

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: machaudio-node
spec:
  containers:
  - name: machaudio
    image: machaudio:latest
    resources:
      limits:
        cpu: "4.0" # Entrypoint will auto-spawn 3 workers
      requests:
        cpu: "4.0"
    ports:
    - containerPort: 8000
      name: base-port
    - containerPort: 8001
    - containerPort: 8002
```

## 4. Graceful Shutdown

The `docker-entrypoint.sh` script uses the `exec` command to hand off PID 1 to the MachAudio supervisor process.
When your orchestrator scales down or terminates the container, the `SIGTERM` signal is passed directly to the supervisor process. The supervisor will then gracefully shut down the `libuv` loops in all child workers, ensuring audio streams are flushed before exiting.
