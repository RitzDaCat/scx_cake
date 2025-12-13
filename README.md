# scx_cake

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Kernel: 6.12+](https://img.shields.io/badge/Kernel-6.12%2B-green.svg)](https://kernel.org)
[![sched_ext](https://img.shields.io/badge/sched__ext-scheduler-orange.svg)](https://github.com/sched-ext/scx)
[![Rust](https://img.shields.io/badge/Rust-stable-brown.svg)](https://www.rust-lang.org)

> **EXPERIMENTAL** - This scheduler is under active development. Use at your own risk.

A **sched_ext** CPU scheduler that applies network bufferbloat concepts from [CAKE](https://www.bufferbloat.net/projects/codel/wiki/Cake/) (Common Applications Kept Enhanced) to CPU scheduling, optimized for **gaming and interactive workloads**.

---

## Table of Contents

- [Why scx_cake?](#why-scx_cake)
- [How It Works (Simple Explanation)](#how-it-works-simple-explanation)
- [The 6-Tier System](#the-6-tier-system)
- [The Sparse Score System](#the-sparse-score-system)
- [Direct Dispatch Optimization](#direct-dispatch-optimization)
- [Wait Budget System](#wait-budget-system)
- [Configuration Options](#configuration-options)
- [Performance Results](#performance-results)
- [Quick Start](#quick-start)
- [Understanding the TUI](#understanding-the-tui)
- [Architecture](#architecture)

---

## Why scx_cake?

Traditional CPU schedulers optimize for **throughput** (getting lots of work done). But gaming needs **low latency** (responding quickly to input).

scx_cake is designed specifically for gaming:

| Traditional Scheduler | scx_cake |
|-----------------------|----------|
| Treats all tasks equally | Prioritizes input handlers |
| Queues tasks fairly | Direct dispatch to idle CPUs |
| One priority level | 6 priority tiers |
| Fixed time slices | Adaptive slices based on behavior |

---

## How It Works (Simple Explanation)

Imagine a busy restaurant kitchen:

1. **Regular kitchens:** Orders wait in one big line. A 30-minute steak order blocks a 2-minute salad order.

2. **scx_cake kitchen:** 
   - Quick orders (salads) go to a fast lane
   - Slow orders (steaks) go to a regular lane
   - Fast lane is always served first
   - If a quick order waits too long, the chef drops everything and makes it immediately

**In CPU terms:**
- **Quick orders** = Input handlers, game threads (run for microseconds)
- **Slow orders** = Compilers, encoders (run for seconds)
- **Fast lane** = Higher priority dispatch queues
- **Chef drops everything** = Preemption safety net

---

## The 6-Tier System

scx_cake sorts tasks into 6 priority tiers based on their behavior:

| Tier | Score Range | Example Tasks | Dispatch Priority |
|------|-------------|---------------|-------------------|
| **Realtime** | 100 | Input handlers, IRQ threads | 1st (highest) |
| **Critical** | 90-99 | Audio, compositor | 2nd |
| **Gaming** | 70-89 | Game threads, UI | 3rd |
| **Interactive** | 50-69 | Normal apps | 4th |
| **Batch** | 30-49 | Nice > 0, heavy apps | 5th |
| **Background** | 0-29 | Compilers, encoders | 6th (lowest) |

### How Tasks Get Their Tier

Each task has a **sparse score** from 0-100. Higher = more responsive, lower = more bulk-work.

```
Score >= 100  →  Realtime tier   (perfect behavior)
Score >= 90   →  Critical tier   (very responsive)
Score >= 70   →  Gaming tier     (responsive)
Score >= 50   →  Interactive tier (normal)
Score >= 30   →  Batch tier      (slower)
Score < 30    →  Background tier (bulk work)
```

### Tier Slice Multipliers

Higher tiers get **smaller** time slices (more responsive):

| Tier | Multiplier | Slice (2ms quantum) | Why |
|------|------------|---------------------|-----|
| Realtime | 0.8x | 1.6ms | Shortest - yields quickly, lets others run |
| Critical | 0.9x | 1.8ms | Short |
| Gaming | 1.0x | 2.0ms | Baseline |
| Interactive | 1.1x | 2.2ms | Slightly longer |
| Batch | 1.2x | 2.4ms | Longer |
| Background | 1.3x | 2.6ms | Longest - less context switching |

---

## The Sparse Score System

The **sparse score** measures how "bursty" a task is. Gaming and input tasks wake frequently but run briefly. Compilers wake rarely but run for a long time.

### The Formula

```
Every time a task stops running:
  
  IF runtime < threshold (default 200µs):
      score += 4   (task is bursty/sparse)
  ELSE:
      score -= 6   (task is bulk/heavy)
  
  Clamp score to 0-100
```

### Why +4/-6?

- **Asymmetric gain/loss:** Tasks fall faster than they climb
- **Prevents tier inflation:** Occasional long runs quickly demote tasks
- **13 short runs** needed to climb from 50 to 100
- **1 long run** drops score by 6 points

### Example: Game Thread

```
Game thread wakes, runs for 50µs → score += 4 (now 54)
Game thread wakes, runs for 80µs → score += 4 (now 58)
...repeat...
After 13 short runs → score = 100 (Realtime tier!)

Then one long frame takes 500µs → score -= 6 (now 94, Critical tier)
```

### Configuration

The **sparse threshold** determines what counts as "short":

```
threshold = quantum × sparse_threshold / 1000

With quantum=2000µs and sparse_threshold=50‰:
threshold = 2000 × 50 / 1000 = 100µs
```

Tasks running under 100µs gain points. Over 100µs lose points.

---

## Direct Dispatch Optimization

**99.8% of tasks bypass queuing entirely** through direct dispatch.

### How It Works

```
Task wakes up
    ↓
Check: Is there an idle CPU?
    ↓
YES → Direct dispatch! Task runs IMMEDIATELY on that CPU
    ↓
NO  → Enqueue to tier's DSQ, wait for dispatch
```

### Why This Matters

- **Zero queue time** for most tasks
- **No contention** with other tasks
- **Immediate execution** when CPUs available

With a 16-core 9800X3D, there's almost always an idle core available!

---

## Wait Budget System

**Borrowed from CAKE's Active Queue Management (AQM).**

If a task waits too long in queue (congestion), it gets demoted to reduce pressure on high-priority tiers.

### Per-Tier Wait Budgets

| Tier | Wait Budget | Meaning |
|------|-------------|---------|
| Realtime | 1ms | If waiting > 1ms, something's wrong |
| Critical | 2ms | Should run within 2ms |
| Gaming | 4ms | Should run within 4ms |
| Interactive | 10ms | More leeway |
| Batch | 35ms | Low priority, can wait |

### Violation Tracking

```
If task waits longer than its tier's budget:
    violation_count++
    
If violation_count >= 4:
    Demote task to next lower tier
    Score -= 30  (significant penalty)
    Reset violation counter
```

This prevents tier congestion by pushing misbehaving tasks down.

---

## Starvation Protection

Ensures no task runs forever without yielding.

### Per-Tier Starvation Limits

| Tier | Limit | Meaning |
|------|-------|---------|
| Realtime | 2ms | Force preempt if running > 2ms |
| Critical | 4ms | Force preempt if running > 4ms |
| Gaming | 8ms | Force preempt if running > 8ms |
| Interactive | 12ms | Force preempt if running > 12ms |
| Batch | 40ms | Force preempt if running > 40ms |
| Background | 100ms | Force preempt if running > 100ms |

If a task exceeds its limit, the scheduler forcibly preempts it.

---

## Configuration Options

### Command Line

| Option | Default | Description |
|--------|---------|-------------|
| `--quantum` | 2000 µs | Base time slice |
| `--sparse-threshold` | 50‰ | Runtime threshold for sparse detection |
| `--input-latency` | 1000 µs | Safety net preemption ceiling |
| `--new-flow-bonus` | 8000 µs | Extra time for freshly woken tasks |
| `--starvation` | 100000 µs | Global starvation limit |
| `-v, --verbose` | false | Enable TUI with live statistics |
| `--interval` | 1 sec | Stats update interval |
| `--debug` | false | Enable BPF debug output |

### Mode Selection

| Mode | Command | Use Case |
|------|---------|----------|
| **Production** | `sudo ./start.sh` | Maximum performance, no overhead |
| **Monitoring** | `sudo ./start.sh -v` | TUI with live stats for tuning |

**Production mode skips all statistics collection** → zero atomic overhead.

---

## Performance Results

### Test System

- **CPU:** AMD Ryzen 9 9800X3D (Zen 5)
- **GPU:** NVIDIA RTX 4090
- **RAM:** 96GB DDR5
- **OS:** CachyOS with kernel 6.18
- **Games:** Arc Raiders, World of Warcraft

### Arc Raiders Results

| Metric | Value | Notes |
|--------|-------|-------|
| **1% Lows** | 180+ fps | Consistent across all scenarios |
| **Average FPS** | 200+ | Limited by game engine |
| **Direct Dispatch** | 99.8% | Nearly all tasks hit idle CPUs |

### World of Warcraft Results (Mythic+ Dungeon)

| Metric | Value |
|--------|-------|
| **Session** | 33 minutes |
| **Dispatches** | 288M |
| **New-flow %** | 99.8% |
| **Max Wait** | 2.3ms |
| **Avg Wait** | 0µs |

---

## Quick Start

### Build

```bash
git clone https://github.com/RitzDaCat/scx_cake.git
cd scx_cake
./build.sh
```

### Run (Production)

```bash
sudo ./start.sh
```

### Run (Monitoring)

```bash
sudo ./start.sh -v
```

### Verify Active

```bash
cat /sys/kernel/sched_ext/root/ops
# Should output: cake
```

---

## Understanding the TUI

Run with `-v` to see live statistics:

```
=== scx_cake Statistics (Uptime: 33m 29s) ===

Dispatches: 288606191 total (99.8% new-flow)

Tier           Dispatches    Max Wait    WaitDemote  StarvPreempt
─────────────────────────────────────────────────────────────────
Realtime           572319      2345 µs             0            29
Critical            76147      2120 µs             0          4687
Gaming              12443      2113 µs             0          3726
Interactive          3615      1932 µs             0          1527
Batch                1921      5313 µs             0            20
Background          28004      1487 µs             0          5530

Sparse flow: +68343 promotions, -67688 demotions, 0 wait-demotes
Input: 263805 events tracked, 36205 preempts fired
Wait time: avg 0 µs, max 5313 µs
```

### What the Metrics Mean

| Metric | Good Value | Meaning |
|--------|------------|---------|
| **new-flow %** | 99%+ | Tasks hitting idle CPUs |
| **Max Wait** | < 5ms | Worst-case queue time |
| **WaitDemote** | 0 | No congestion-based demotions |
| **StarvPreempt** | Low | Tasks not exceeding limits |
| **promotions ≈ demotions** | Balanced | Stable tier distribution |

---

## Architecture

### Project Structure

```
scx_cake/
├── src/
│   ├── main.rs          # Userspace controller, CLI
│   ├── stats.rs         # Tier names constant
│   ├── tui.rs           # Terminal UI with ratatui
│   └── bpf/
│       ├── intf.h       # Shared types (tiers, stats)
│       └── cake.bpf.c   # BPF scheduler logic
├── build.sh             # Build helper
└── start.sh             # Run helper
```

### BPF Callbacks

| Callback | Purpose |
|----------|---------|
| `cake_select_cpu` | Find idle CPU, direct dispatch |
| `cake_enqueue` | Classify tier, route to DSQ |
| `cake_dispatch` | Pull from DSQs in priority order |
| `cake_running` | Track wait time, wait budget |
| `cake_stopping` | Update sparse score, charge vtime |
| `cake_tick` | Starvation protection |

### Key Data Structures

```c
struct cake_task_ctx {
    u64 deficit;        // Time owed to this task
    u64 last_run_at;    // When task last ran
    u64 total_runtime;  // Accumulated runtime
    u32 sparse_score;   // 0-100, behavior score
    u8  tier;           // Current priority tier
    u8  wait_violations; // Consecutive budget violations
};
```

---

## Requirements

| Requirement | Version |
|-------------|---------|
| **Kernel** | Linux 6.12+ with `CONFIG_SCHED_CLASS_EXT=y` |
| **Distros** | CachyOS, Arch (with sched-ext kernel) |
| **Rust** | Stable (1.70+) |
| **Clang** | 10+ |

---

## Warm-Up Period

**Important:** When the scheduler starts, tasks begin with score=50 (Interactive tier). It takes **~5-10 seconds** for tasks to climb to their natural tiers through the sparse score system.

During warm-up:
- Game threads may show briefly lower 1% lows
- Input handlers not yet in Realtime tier
- This is expected behavior, not a bug

After warm-up, tasks settle into stable tiers based on actual behavior.

---

## License

GPL-2.0 - See [LICENSE](LICENSE) for details.

---

## Acknowledgments

- [CAKE qdisc](https://www.bufferbloat.net/projects/codel/wiki/Cake/) for the bufferbloat-fighting inspiration
- [sched_ext](https://github.com/sched-ext/scx) team for making BPF schedulers possible
- CachyOS community for testing and feedback
