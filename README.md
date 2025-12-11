# scx_cake

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Kernel: 6.12+](https://img.shields.io/badge/Kernel-6.12%2B-green.svg)](https://kernel.org)
[![sched_ext](https://img.shields.io/badge/sched__ext-scheduler-orange.svg)](https://github.com/sched-ext/scx)
[![Rust](https://img.shields.io/badge/Rust-stable-brown.svg)](https://www.rust-lang.org)

> **EXPERIMENTAL** - This scheduler is under active development. Use at your own risk. Performance may vary by workload and system configuration.

A **sched_ext** CPU scheduler that applies network bufferbloat concepts from [CAKE](https://www.bufferbloat.net/projects/codel/wiki/Cake/) (Common Applications Kept Enhanced) to CPU scheduling, optimized for **gaming and interactive workloads**.

---

## Table of Contents

- [Why scx_cake?](#why-scx_cake)
- [Performance Results](#performance-results)
- [Design Philosophy](#design-philosophy)
- [How It Works](#how-it-works)
- [Configuration](#configuration)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Testing Methodology](#testing-methodology)
- [Known Limitations](#known-limitations)
- [Roadmap](#roadmap)

---

## Why scx_cake?

Modern games demand consistent frame times and responsive input. Traditional schedulers optimize for throughput, not latency. scx_cake flips this around:

- **Prioritizes input-handling tasks** with guaranteed latency ceiling
- **Direct dispatch** to idle CPUs bypasses queuing entirely
- **Sparse flow detection** identifies gaming workloads automatically
- **Safety net preemption** ensures input never waits too long

---

## Performance Results

### Test System

- **CPU:** AMD Ryzen 9 9800X3D (Zen 5)
- **GPU:** NVIDIA RTX 4090
- **RAM:** 96GB DDR5
- **OS:** CachyOS with kernel 6.18
- **Game:** Arc Raiders (Unreal Engine 5)

### Metrics Summary

| Metric | Value | Notes |
|--------|-------|-------|
| **1% Lows** | 180+ fps | Consistent across standing and active gameplay |
| **Direct Dispatch** | 85%+ | Most wakes find idle CPUs |
| **Gaming Tier** | 15-20% | Interactive tasks correctly prioritized |
| **Input Events** | ~10k/sec | Mouse, keyboard, and ultra-fast tasks tracked |
| **Safety Net Preempts** | ~12% | Catches tasks blocked >1ms |
| **Wait Time** | 0µs avg | No queue delays |

---

## Design Philosophy

### Inspiration: CAKE for Networks, CAKE for CPUs

The CAKE qdisc revolutionized network bufferbloat by recognizing that **not all packets are equal**. Interactive traffic (gaming, VoIP) needs low latency, while bulk transfers (downloads) need throughput.

scx_cake applies the same principle to CPU scheduling:

| CAKE (Network) | scx_cake (CPU) |
|----------------|----------------|
| Sparse flows (VoIP) | Sparse tasks (input handlers) |
| Bulk flows (downloads) | Bulk tasks (compilation) |
| Per-flow fairness | Per-task fairness |
| Deficit Round Robin | Quantum-based scheduling |
| Flow prioritization | Tier-based dispatch queues |

### Key Insight

Gaming workloads have a distinctive pattern:

- **Short bursts** of CPU activity (input events, frame logic)
- **Frequent wakes** (vsync, audio callbacks)
- **Low total runtime** relative to wake frequency

scx_cake detects this "sparse" behavior and gives these tasks priority dispatch.

---

## How It Works

### Direct Local Dispatch (Fast Path)

When a task wakes up:

```
Task wakes → Check for idle CPU → Found? → RUN IMMEDIATELY
                                    ↓
                               Not found? → Queue to DSQ
```

Approximately **85% of wakes** find an idle CPU and run instantly with zero queue time. Uses `SCX_ENQ_LAST` flag to skip redundant processing, saving 5-20µs per dispatch.

### Sparse Flow Classification

Tasks are continuously scored based on runtime behavior:

```c
// Sparse score increases when runtime is short relative to quantum
sparse_score = (quantum_ns - runtime) / quantum_ns * scale

// Tasks with score >= 70 are classified as "sparse" (interactive)
if (sparse_score >= SPARSE_PROMOTE_THRESHOLD)
    route to GAMING_DSQ  // Dispatched first!
else
    route to NORMAL_DSQ  // Dispatched second
```

Game threads, input handlers, and audio callbacks automatically get priority.

### Two-Tier Dispatch Queue System

```
┌─────────────┐     ┌─────────────┐
│ GAMING_DSQ  │ ──▶ │   CPU       │  (Served FIRST)
│ (sparse)    │     │             │
└─────────────┘     │             │
                    │             │
┌─────────────┐     │             │
│ NORMAL_DSQ  │ ──▶ │             │  (Served SECOND)
│ (bulk)      │     └─────────────┘
└─────────────┘
```

GAMING_DSQ is always consumed first, ensuring interactive tasks preempt bulk work.

### Input-Specific Safety Net

For tasks identified as **likely input handlers** (runtime <30µs, sparse score ≥80):

```c
// If input task hasn't run in > input_latency_ns, preempt immediately
if (is_likely_input_task(tctx)) {
    time_since_run = now - last_run_at;
    if (time_since_run > input_latency_ns) {
        kick_cpu(prev_cpu, PREEMPT);  // Force reschedule
    }
}
```

This guarantees a maximum input latency ceiling (default 1ms, configurable).

### New Flow Bonus

Fresh tasks (recently woken) get extended time slices:

```c
// New flows get quantum + bonus (e.g., 2ms + 8ms = 10ms)
slice = quantum_ns + new_flow_bonus_ns;
```

This gives input event handlers more runway before preemption.

---

## Configuration

### Command Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--quantum` | 2000 µs | Base time slice per task |
| `--sparse-threshold` | 50‰ | Runtime threshold for sparse detection (5% of quantum) |
| `--input-latency` | 1000 µs | Maximum input latency before safety net preempts |
| `--new-flow-bonus` | 8000 µs | Extra time for fresh tasks |
| `--starvation` | 100000 µs | Max time before forced preemption |
| `--verbose` | false | Print statistics every second |
| `--interval` | 1 sec | Stats print interval |

### Tuning Guide

#### Sparse Threshold

Controls which tasks get Gaming DSQ priority:

| Threshold | Cutoff (2ms quantum) | Effect |
|-----------|----------------------|--------|
| 10‰ | 20µs | Very strict - only IRQ handlers |
| 25‰ | 50µs | Strict - input + light game logic |
| **50‰** | 100µs | **Balanced (recommended)** |
| 100‰ | 200µs | Lenient - most game threads |

#### Input Latency

Controls the safety net ceiling:

| Value | Meaning | Use Case |
|-------|---------|----------|
| 250µs | Aggressive | Competitive esports |
| 500µs | Tight | Fast-paced games |
| **1000µs** | **Conservative (default)** | General gaming |
| 2000µs | Relaxed | Background gaming |

---

## Verbose Statistics

Run with `--verbose` to see real-time metrics:

```
=== scx_cake Statistics ===
Dispatches: 15368979 total (86.0% new-flow)
  Voice:       389
  Gaming:      2072365
  Best Effort: 83827
  Background:  0
Sparse flow: +3183 promotions, -1960 demotions
Input: 971269 events tracked, 117623 preempts fired
Wait time: avg 0 µs, max 0 µs
```

### Metric Interpretation

| Metric | Good Value | Meaning |
|--------|------------|---------|
| **new-flow %** | 80-90% | Most tasks dispatched directly to idle CPUs |
| **Gaming tier** | 15-25% | Interactive tasks correctly classified |
| **Input events** | 5-15k/sec | Ultra-fast tasks detected |
| **Preempts fired** | 10-15% of events | Safety net activating when needed |
| **Wait time** | 0-10µs | No queue delays |

---

## Architecture

### Project Structure

```
scx_cake/
├── Cargo.toml              # Rust dependencies
├── build.rs                # BPF compilation via scx_cargo
├── build.sh                # Build helper script
├── start.sh                # Run helper script
└── src/
    ├── main.rs             # Userspace: CLI, BPF loading, statistics
    ├── stats.rs            # Statistics display utilities
    └── bpf/
        ├── intf.h          # Shared types between BPF and Rust
        └── cake.bpf.c      # Kernel BPF scheduler logic
```

### Key Components

#### cake.bpf.c - Kernel Scheduler

| Function | Purpose |
|----------|---------|
| `cake_select_cpu()` | CPU selection with direct dispatch |
| `cake_enqueue()` | DSQ routing based on sparse classification |
| `cake_dispatch()` | Priority dispatch (Gaming → Normal) |
| `is_likely_input_task()` | Runtime heuristic for input detection |
| `update_sparse_score()` | Adaptive scoring based on runtime |

#### intf.h - Shared Data Structures

| Structure | Purpose |
|-----------|---------|
| `cake_task_ctx` | Per-task state (deficit, runtime, sparse score) |
| `cake_stats` | Global statistics counters |
| `cake_tier` | Priority tier enumeration |

#### main.rs - Userspace Controller

- CLI argument parsing via `clap`
- BPF skeleton loading via `libbpf-rs`
- Statistics display and signal handling

---

## Requirements

| Requirement | Version |
|-------------|---------|
| **Kernel** | Linux 6.12+ with `CONFIG_SCHED_CLASS_EXT=y` |
| **Distros** | CachyOS, Arch (with sched-ext kernel), or custom kernel |
| **Rust** | Stable toolchain (1.70+) |
| **Clang** | 10+ for BPF compilation |
| **libbpf** | Included via scx_utils |

---

## Quick Start

### Build

```bash
git clone https://github.com/RitzDaCat/scx_cake.git
cd scx_cake
./build.sh
```

### Run

```bash
# Default settings (recommended)
sudo ./start.sh

# With verbose statistics
sudo ./start.sh --verbose

# Custom tuning
sudo ./start.sh --quantum 2000 --sparse-threshold 50 --input-latency 500 --verbose
```

### Verify

Check that the scheduler is active:

```bash
cat /sys/kernel/sched_ext/root/ops
# Should output: cake
```

---

## Testing Methodology

### Test Protocol

1. **Baseline:** Standing idle in firing range (5 minutes)
2. **Stress:** Spamming A/D movement keys (30 seconds)
3. **Gameplay:** Active combat scenarios (5 minutes)

### Results Summary

| Test | 1% Lows | Avg FPS | Notes |
|------|---------|---------|-------|
| Standing | 180+ | 200+ | Excellent consistency |
| Key spam | 165+ | 195+ | Minimal degradation under input stress |
| Combat | 170+ | 190+ | Stable during intensive scenes |

---

## Known Limitations

| Limitation | Description |
|------------|-------------|
| **Experimental** | Not ready for production systems |
| **Input heuristic** | Uses runtime patterns, not actual evdev hooks |
| **Gaming-focused** | May not be optimal for server workloads |
| **Single-socket** | Not tested on multi-socket NUMA systems |

---

## Roadmap

- [ ] Full 4-tier priority (Voice, Gaming, Best Effort, Background)
- [ ] Per-CPU statistics (reduce atomic contention)
- [ ] CPU affinity awareness for gaming processes
- [ ] Deadline-based scheduling for audio
- [ ] Integration with game process detection (PID or cgroup)

---

## Contributing

Contributions, testing, and feedback welcome!

- **Issues:** Bug reports with verbose stats appreciated
- **Testing:** Try different games and report 1% lows
- **Ideas:** Open discussions for new features

---

## License

GPL-2.0 - See [LICENSE](LICENSE) for details.

---

## Acknowledgments

- [CAKE qdisc](https://www.bufferbloat.net/projects/codel/wiki/Cake/) for the bufferbloat-fighting inspiration
- [sched_ext](https://github.com/sched-ext/scx) team for making BPF schedulers possible
- CachyOS community for testing and feedback
