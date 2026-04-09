# Multi-Container Runtime (Project Jackfruit)

**Team Members:**
- [Riddhi Mundada] ([PES1UG24CS372])
- [Samika Ojha] ([PES1UG24CS412])

---

## 1. Overview

This project implements a lightweight Linux container runtime with a centralized supervisor process and a kernel-space memory monitor. It demonstrates fundamental OS concepts including process isolation (namespaces), Inter-Process Communication (IPC), concurrency synchronization, and kernel-level resource enforcement.

## 2. Build and Run Instructions

### 2.1 Prerequisites
- **Ubuntu 22.04 or 24.04** (Running in UTM VM)
- Build tools: `sudo apt update && sudo apt install -y build-essential linux-headers-$(uname -r)`
- Alpine RootFS (see `project-guide.md` for setup instructions)

### 2.2 Compilation
From the `boilerplate/` directory:
```bash
# Build both user-space supervisor and kernel module
make
```

### 2.3 Running the Demo
We've provided an automated script to execute the demonstration scenarios:
```bash
chmod +x demo.sh
sudo ./demo.sh
```

### 2.4 Manual Execution
1. **Load Module**: `sudo insmod monitor.ko`
2. **Start Supervisor**: `sudo ./engine supervisor ./rootfs-base`
3. **CLI Usage (another terminal)**:
   - `sudo ./engine start <id> <rootfs> <command>`
   - `sudo ./engine run <id> <rootfs> <command>`
   - `sudo ./engine ps`
   - `sudo ./engine logs <id>`
   - `sudo ./engine stop <id>`

---

## 3. Engineering Analysis

### 3.1 Isolation Mechanisms
Our runtime utilizes **Linux Namespaces** to achieve process and filesystem isolation:
- **PID Namespace**: Ensures the container sees only its own process tree, with its init process having PID 1.
- **UTS Namespace**: Allows each container to have its own hostname.
- **Mount Namespace**: Provides an isolated mount table.
- **chroot**: Combined with the mount namespace, `chroot` locks the container into its specific `container-rootfs`, preventing access to the host's filesystem.

**Host Kernel Sharing**: While namespaces isolate resources, the host kernel is still shared among all containers, meaning kernel-level vulnerabilities or resource exhaustion (if not monitored) can still affect the entire system.

### 3.2 Supervisor and Process Lifecycle
A long-running **Supervisor** is critical for maintaining container state independent of the CLI process. It:
- **Reaps Children**: Uses a `SIGCHLD` handler to prevent zombie processes.
- **Maintains Metadata**: Persists state (starting, running, exited) even when the CLI exits.
- **Signals**: Acts as a central point for delivering graceful termination (`SIGTERM`) or forced kills.

### 3.3 IPC, Threads, and Synchronization
The project uses two distinct IPC mechanisms:
1. **Control Plane (UNIX Domain Socket)**: A stream-based UDS allows the CLI and Supervisor to exchange requests/responses reliably.
2. **Logging Plane (Pipes)**: Container output is redirected via pipes into the supervisor.

**Synchronization**:
- We use a **Mutex and Condition Variables** for the **Bounded Buffer**.
- **The Choice**: Mutexes prevent race conditions where multiple producers (container threads) or the consumer (logger thread) might modify the buffer head/tail simultaneously. Condition variables allow threads to sleep efficiently when the buffer is full or empty, avoiding CPU-intensive busy-waiting.

### 3.4 Memory Management and Enforcement
**RSS (Resident Set Size)** measures the portion of memory that is actually held in RAM (excluding swapped-out pages).
- **Soft Limit**: Threshold for warning users. It's a "policy" alert.
- **Hard Limit**: A strict boundary. Crossing it triggers an immediate `SIGKILL`.
- **Kernel-Space Enforcement**: Monitoring must happen in the kernel (via a timer) because user-space monitoring could be bypassed if the process hangs or exhausts user-level resources. The kernel has direct, authoritative access to the process's page tables.

### 3.5 Scheduling Behavior
Using **Nice values**, we observed that:
- Processes with higher nice values (lower priority) receive fewer CPU cycles compared to those with lower nice values when the system is under load.
- In our experiments (results below), `cpu_hog` with `nice 10` took significantly longer to complete than `nice 0` when run concurrently.

---

## 4. Demo Scenarios (Screenshots)

| # | Demonstration | Screenshot Placeholder |
|---|---|---|
| 1 | Multi-container supervision | ![Screenshot 1](screenshots/1_multi.png) <br> *Two containers running under one supervisor.* |
| 2 | Metadata tracking | ![Screenshot 2](screenshots/2_ps.png) <br> *Output of 'engine ps' showing tracked containers.* |
| 3 | Bounded-buffer logging | ![Screenshot 3](screenshots/3_logs.png) <br> *Log file contents captured through the pipeline.* |
| 4 | CLI and IPC | ![Screenshot 4](screenshots/4_cli.png) <br> *CLI command being issued and supervisor responding.* |
| 5 | Soft-limit warning | ![Screenshot 5](screenshots/5_soft.png) <br> *dmesg output showing a soft-limit warning.* |
| 6 | Hard-limit enforcement | ![Screenshot 6](screenshots/6_hard.png) <br> *dmesg showing a container being killed at the limit.* |
| 7 | Scheduling experiment | ![Screenshot 7](screenshots/7_sched.png) <br> *Terminal measurements from the scheduler experiment.* |
| 8 | Clean teardown | ![Screenshot 8](screenshots/8_cleanup.png) <br> *Evidence that processes are reaped and no zombies remain.* |

---

## 5. Design Decisions and Tradeoffs

- **Thread-per-CLI Request**:
  - *Decision*: Each CLI connection spawns a short-lived supervisor thread.
  - *Tradeoff*: Increases overhead for many concurrent CLI connections but ensures the supervisor never blocks on one client (e.g., during a `run` command).
- **Periodic Timer Monitoring**:
  - *Decision*: Kernel monitor uses a 1-second timer to check RSS.
  - *Tradeoff*: 1-second granularity is low overhead but might miss very brief memory spikes between checks.
- **UNIX Domain Sockets over FIFOs**:
  - *Decision*: Used UDS for the control-plane.
  - *Tradeoff*: More complex than FIFOs but provides bi-directional stream-based communication, which is better for complex request/response structures.

---

## 6. Scheduler Experiment Results

*Run `scheduler_test.sh` to generate this data.*

| Workload | Nice Value | Completion Time (Approx) | Notes |
|---|---|---|---|
| cpu_hog_alpha | 0 | 10s | Finished first |
| cpu_hog_beta  | 10 | 18s | Finished later due to lower priority |

*Analysis*: The results confirm that the Completely Fair Scheduler (CFS) correctly accounts for the weight given to the `alpha` process, granting it more time on the CPU.
