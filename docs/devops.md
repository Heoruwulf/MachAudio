# MachAudio DevOps & Deployment Guide

This document outlines best practices for deploying MachAudio in containerized environments, specifically focusing on Kubernetes (K8s) and Compute Instances, to maintain its ultra-low latency guarantees.

## OS Optimization & Containerization

MachAudio is designed for high performance, utilizing features like CPU pinning, real-time thread scheduling (`SCHED_FIFO`), and CPU governor tuning. However, applying these optimizations inside a standard, non-privileged Docker container is restrictive.

**Rule of Thumb:** Manage physical hardware states (Governors, C-States) at the host infrastructure level, and manage process scheduling and concurrency via the MachAudio container's command-line arguments.

### 1. Host-Level Tuning

You should not add core governor or DMA latency tuning directly to the `Dockerfile`. These require `CAP_SYS_ADMIN` or root privileges and affect the entire host, which is a bad practice for multi-tenant container environments.

*   **Compute Instance (Bare Metal/VM):** Run the provided `scripts/tune-host.sh` on the host machine before starting Docker. This ensures the physical cores are already locked in `performance` mode.
*   **Kubernetes:** Use a **DaemonSet** or the **Node Tuning Operator (TuneD)** to apply the `cpu-partitioning` or `realtime` profiles to your worker nodes. This ensures the OS BIOS/Kernel settings are optimized for low latency before pods are scheduled.

### 2. Application-Level Tuning (CLI Switches)

Even with the host tuned, you still need to instruct MachAudio on how to utilize the tuned environment.

*   **`--core-mask <mask|csv>`:** You must dictate the concurrency model and core topology. If you reserved 4 tuned cores on the host (e.g. cores 2,3,4,5), pass `--core-mask 2,3,4,5`. MachAudio will explicitly lock its worker processes to these specific physical cores to prevent cache invalidation.
    *   *K8s Exception:* If using Kubernetes with the **CPU Manager** (`static` policy), K8s handles the pinning dynamically, so you can omit this switch. If you are *not* using CPU Manager but still want unpinned workers, you can pass a list of `none`s (e.g. `--core-mask none,none,none,none`).
*   **`--rt-priority <N>`:** Host tuning prepares the hardware, but this switch tells the Linux Scheduler to treat your specific application as mission-critical. Setting this upgrades the process to the `SCHED_FIFO` real-time class, preempting almost any other normal process on that core.
    *   *Requirement:* The container must be run with the `CAP_SYS_NICE` capability.

## Deployment Strategies

### Compute Instance (Docker)

If you are deploying to a dedicated VM or bare metal server where you manage the host:

1.  Tune the host cores first (e.g., cores 2, 3, 4, 5):
    ```bash
    sudo ./scripts/tune-host.sh 2
    sudo ./scripts/tune-host.sh 3
    # ...
    ```
2.  Run the container, granting `SYS_NICE` for real-time priority, and explicitly pinning the cores:
    ```bash
    docker run -d \
      --name machaudio \
      --network host \
      --cap-add=SYS_NICE \
      machaudio --core-mask 2,3,4,5 --rt-priority 90
    ```

### Kubernetes (K8s)

For deploying MachAudio in Kubernetes, utilize the CPU Manager to handle core isolation.

1.  Ensure the Kubelet on your low-latency nodes is configured with the `CPUManager` feature gate and the `static` policy.
2.  Ensure your pod specifications request **Guaranteed QoS** (Requests must equal Limits, and CPU must be an integer).
3.  Add the `SYS_NICE` capability to the security context.

**Example `deployment.yaml` snippet:**

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: machaudio
spec:
  template:
    spec:
      containers:
      - name: machaudio
        image: machaudio:latest
        command: ["machaudio", "--core-mask", "0,1,2,3", "--rt-priority", "90"]
        # In K8s, workers use these logical indices while K8s handles physical pinning.
        resources:
          requests:
            cpu: "4"      # Integer required for Guaranteed QoS
            memory: "1Gi"
          limits:
            cpu: "4"      # Must equal requests
            memory: "1Gi" # Must equal requests
        securityContext:
          capabilities:
            add: ["SYS_NICE"]
```

## Dockerfile Optimizations

While OS tuning belongs outside the Dockerfile, build-time optimizations are still valuable. The provided Dockerfile uses Alpine and builds a highly optimized release binary.

*   **AVX2:** The project's CMake configuration enforces `-mavx2` by default. Ensure your deployment nodes support this instruction set. If deploying to specific, newer hardware (e.g., AWS Ice Lake instances), consider updating the `CMakeLists.txt` to use `-march=skylake-avx512` for even greater vectorization performance.
