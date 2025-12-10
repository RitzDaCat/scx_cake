# scx_cake

> ⚠️ **EXPERIMENTAL** - This scheduler is under active development. Use at your own risk.

A sched_ext scheduler inspired by the CAKE (Common Applications Kept Enhanced) bufferbloat algorithm. This scheduler applies CAKE's queue management concepts to CPU scheduling for improved gaming and interactive performance.

## Performance

Early testing shows **excellent 1% lows** in games like Arc Raiders compared to default schedulers.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      scx_cake                               │
├─────────────────────────────────────────────────────────────┤
│  Userspace (Rust)          │  Kernel (BPF)                  │
│  ├── CLI parsing           │  ├── select_cpu                │
│  ├── BPF loading           │  ├── enqueue                   │
│  ├── Config injection      │  ├── dispatch                  │
│  └── Stats display         │  ├── running/stopping          │
│                            │  └── tick                      │
└─────────────────────────────────────────────────────────────┘
```

## Design Philosophy

This scheduler adapts CAKE's network queue management principles to CPU scheduling:

| CAKE (Network) | scx_cake (CPU) | Implementation |
|----------------|----------------|----------------|
| Packet | Scheduling timeslice | `SCX_SLICE_DFL` (default ~20ms) |
| Flow | Task/Process | Per-task context in BPF map |
| Bandwidth | CPU time | Runtime tracking |
| Sparse flow | Interactive task | Short bursts, high wake frequency |
| Bulk flow | CPU-bound task | Long continuous runtime |
| Queue delay | Wake-to-run latency | Tracked in stats |

## Code Structure

```
scx_cake/
├── Cargo.toml              # Rust dependencies
├── build.rs                # BPF compilation (uses scx_cargo)
├── src/
│   ├── main.rs             # Userspace: CLI, BPF loading, stats
│   ├── stats.rs            # Statistics utilities
│   └── bpf/
│       ├── intf.h          # Shared types (BPF ↔ Rust)
│       └── cake.bpf.c      # Kernel BPF scheduler logic
├── build.sh                # Build helper script
└── start.sh                # Run helper script
```

## How It Works

### 1. Task Context (`cake_task_ctx`)

Each task has per-task storage tracking:

```c
struct cake_task_ctx {
    u64 deficit;        // Time owed to this task (ns)
    u64 last_run_at;    // Last time task ran
    u64 total_runtime;  // Total accumulated runtime
    u64 last_wake_at;   // Last wakeup time
    u32 wake_count;     // Number of wakeups
    u32 run_count;      // Number of times scheduled
    u32 sparse_score;   // 0-100, higher = more interactive
    u8  tier;           // Priority tier (0-3)
    u8  flags;          // Flow state flags
};
```

### 2. Sparse Flow Detection

The sparse score determines if a task is "interactive" (like games) or "bulk" (like compilation):

```
Runtime < threshold (10% of quantum):
    → sparse_score += 5 (max 100)
    → If score ≥ 70: promoted to Gaming tier

Runtime ≥ threshold:
    → sparse_score -= 5 (min 0)
    → If score < 70: demoted from Gaming tier
```

This automatically identifies:
- **Sparse (score ≥ 70)**: Games, input handlers, audio - get Gaming tier priority
- **Bulk (score < 70)**: Compilers, encoders - stay in Best Effort tier

### 3. Priority Tiers

Tasks are classified into 4 tiers (like CAKE's DiffServ tins):

| Tier | Nice Value | Sparse Score | Use Case |
|------|------------|--------------|----------|
| Voice (0) | ≤ -10 | - | Real-time audio |
| Gaming (1) | < 0 OR - | ≥ 70 | Games, interactive |
| Best Effort (2) | 0-9 | < 70 | Normal applications |
| Background (3) | ≥ 10 | - | Batch jobs |

### 4. Dispatch Flow

```
Task Wakes Up
     │
     ▼
┌────────────────┐
│  select_cpu    │ ─── Find idle CPU (prefer same LLC)
└────────────────┘
     │
     ▼ (if idle CPU found)
┌────────────────┐
│ Direct Dispatch│ ─── scx_bpf_dsq_insert(SCX_DSQ_LOCAL)
└────────────────┘     Skip enqueue, run immediately
     │
     ▼ (if no idle CPU)
┌────────────────┐
│   enqueue      │ ─── Classify tier, update stats
└────────────────┘     Insert to SHARED_DSQ (FIFO)
     │
     ▼
┌────────────────┐
│   dispatch     │ ─── Move from SHARED_DSQ to local
└────────────────┘     CPU runs the task
     │
     ▼
┌────────────────┐
│   running      │ ─── Record start time
└────────────────┘
     │
     ▼
┌────────────────┐
│   stopping     │ ─── Calculate runtime
└────────────────┘     Update sparse score
                       Charge vtime for fairness
```

### 5. Current Simplifications

The current version uses a **single FIFO DSQ** rather than CAKE's full multi-tier DRR++:

- All tasks go to `SHARED_DSQ` (ID 0)
- Tier classification is tracked but not used for dispatch ordering
- This was chosen for stability after multi-DSQ versions had BPF verifier issues

Future versions may add:
- Per-tier DSQs (8 total: 4 tiers × new/old flow lists)
- vtime-based fair scheduling within DSQs
- Bandwidth thresholds per tier

## Configuration

### Command Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--quantum` | 4000 µs | Base scheduling quantum |
| `--new-flow-bonus` | 8000 µs | Bonus time for waking tasks |
| `--sparse-threshold` | 100‰ | CPU usage threshold for sparse (10%) |
| `--starvation-limit` | 100000 µs | Max time before forced preemption |
| `--verbose` | false | Print statistics periodically |
| `--interval` | 1 sec | Stats print interval |

### Tuning for Gaming

For best gaming performance:
```bash
sudo ./start.sh --quantum 2000 --sparse-threshold 50
```

Lower quantum = more responsive but more overhead.
Lower sparse threshold = more tasks classified as sparse.

## Requirements

- **Kernel**: Linux 6.12+ with `CONFIG_SCHED_CLASS_EXT=y`
- **Rust**: Stable toolchain
- **Clang/LLVM**: For BPF compilation

### Arch Linux / CachyOS

These typically have sched_ext enabled by default in recent kernels.

## Building

```bash
./build.sh
```

## Running

```bash
# Run with verbose stats
sudo ./start.sh --verbose

# Or directly
sudo ./target/release/scx_cake --verbose
```

## Statistics Output

```
=== scx_cake Statistics ===
Dispatches: 1234 total (85.2% new-flow)
  Voice:       12
  Gaming:      456
  Best Effort: 766
  Background:  0
Sparse flow: +23 promotions, -5 demotions
Wait time: avg 42 µs, max 1203 µs
```

## License

GPL-2.0

## Contributing

This is an experimental scheduler. Contributions, testing, and feedback welcome!
