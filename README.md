# scx_cake: A Low-Latency Gaming Scheduler

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Kernel: 6.12+](https://img.shields.io/badge/Kernel-6.12%2B-green.svg)](https://kernel.org)
[![sched_ext](https://img.shields.io/badge/sched__ext-scheduler-orange.svg)](https://github.com/sched-ext/scx)

> **ABSTRACT**: `scx_cake` is an experimental BPF CPU scheduler designed for **gaming workloads**. It abandons traditional "Fairness" in favor of strict "Latency Prioritization", reducing total scheduling overhead to approximately **~80-140 CPU cycles per task** (down from ~470 cycles in naive implementations).

---

## Table of Contents

1. [Research Objectives](#1-research-objectives)
   - [Test Platform](#test-platform)

2. [Architecture: The Zero-Cycle Engine](#2-architecture-the-zero-cycle-engine)
   - [Cycle Cost Analysis](#cycle-cost-analysis)
     - [Per-Function Breakdown](#per-function-breakdown)
     - [Call Frequency Estimation](#call-frequency-estimation-gaming-workload-on-9800x3d)
     - [Before vs After Comparison](#comparison-before-vs-after-optimizations)
   - [A. Pre-Computed Math](#a-pre-computed-math-2-cycles)
   - [B. Global Bitmask Search](#b-global-bitmask-search-5-cycles)
   - [C. The Scoreboard](#c-the-scoreboard-neighbor-awareness)
   - [D. Direct Dispatch Bypass](#d-direct-dispatch-bypass-5-cycles)

3. [The 7-Tier Priority System](#3-the-7-tier-priority-system)
   - [Tier Classification Logic](#tier-classification-logic)

4. [Performance Optimizations](#4-performance-optimizations)
   - [Understanding CPU Cycles](#understanding-cpu-cycles)
   - [A. Timestamp Caching](#a-timestamp-caching-scx_bpf_now)
   - [B. Cache Line Optimization](#b-cache-line-optimization)
   - [C. Idle Path Streamlining](#c-idle-path-streamlining)
   - [D. Division-Free Arithmetic](#d-division-free-arithmetic-bitwise-magic)
   - [E. Branchless Programming](#e-branchless-programming)
   - [F. Pre-Computed Values](#f-pre-computed-values)

5. [HPC Patterns & Industry Best Practices](#5-hpc-patterns--industry-best-practices)
   - [Confirmed Patterns](#confirmed-patterns-already-implemented)
   - [Evaluated But Not Implemented](#evaluated-but-not-implemented)
   - [Key References](#key-references)

6. [Experimental Findings](#6-experimental-findings)
   - [A. Performance Wins](#a-performance-wins-)
   - [B. Performance Regressions & Lessons Learned](#b-performance-regressions--lessons-learned-)
   - [C. The "1% Low" Regression (Case Study)](#c-the-1-low-regression-case-study)
   - [D. The "FPS Overshoot" Phenomenon](#d-the-fps-overshoot-phenomenon)
   - [E. Performance Baseline](#e-performance-baseline)

7. [Usage](#7-usage)
   - [Quick Start](#quick-start)
   - [Monitoring (Verbose Mode)](#monitoring-verbose-mode)
   - [CLI Options](#cli-options)

8. [License & Acknowledgments](#8-license--acknowledgments)

9. [Glossary](#9-glossary)

---

## Glossary

Quick reference for technical terms used throughout this document.

| Term | Definition |
|:-----|:-----------|
| **BPF / eBPF** | Extended Berkeley Packet Filter - a technology allowing sandboxed programs to run in kernel space without modifying kernel source |
| **sched_ext** | Linux kernel framework (6.12+) that allows custom CPU schedulers to be implemented as BPF programs |
| **DSQ** | Dispatch Queue - a holding queue where tasks wait before being assigned to a CPU |
| **TZCNT** | "Trailing Zero Count" - x86 hardware instruction (`__builtin_ctzll`) that finds the first set bit in O(1) time |
| **TSC** | Time Stamp Counter - hardware register counting CPU cycles; reading it costs ~15-25 cycles |
| **rq->clock** | Cached timestamp on each CPU's run queue; cheaper to read than TSC (~3-5 cycles) |
| **Vtime** | Virtual Time - a fairness mechanism that normalizes task runtimes; deleted in scx_cake |
| **Sparse Score** | 0-100 metric computed from task behavior; high score = short bursts = high priority tier |
| **Hot Path** | The critical code path executed on every scheduling decision (wakeup → dispatch) |
| **False Sharing** | Performance degradation when CPUs update different variables on the same cache line |
| **Cache Line** | 64-byte block of memory fetched as a unit; crossing lines or sharing causes overhead |
| **L1 Cache** | Fastest CPU cache (~1-4 cycles access); our task context is designed to fit here |
| **Direct Dispatch** | Bypassing the global queue to place a task directly on an idle CPU's local queue |
| **Preemption** | Forcibly stopping a running task to run a higher-priority one |
| **Starvation** | When a task waits too long without running; our starvation limits prevent this |
| **1% Low FPS** | Gaming benchmark metric: the FPS that 99% of frames exceed; indicates stutter/smoothness |
| **Context Switch** | The CPU operation of saving one task's state and loading another's (~1000+ cycles) |
| **Atomic Operation** | CPU instruction that completes indivisibly; used for lock-free concurrent access |
| **Branchless** | Code without if/else jumps; avoids branch misprediction penalty (~15-20 cycles) |
| **Multiply-Shift** | Division approximation using `x * magic >> shift`; avoids slow division instruction |
| **SMT** | Simultaneous Multi-Threading (Hyperthreading); 2 threads share one physical core |
| **NUMA** | Non-Uniform Memory Access; memory access time varies by which CPU socket owns the RAM |
| **IPC** | Instructions Per Cycle; measure of CPU efficiency (higher = better) |
| **Kalman Filter** | Statistical algorithm for estimating values from noisy data; used for runtime prediction |

---

## 1. Research Objectives

Modern Operating System schedulers (CFS, EEVDF) are designed for **Fairness** and **Throughput**.

**The Problem**: In competitive gaming, "Fairness" is detrimental. If the scheduler pauses the Game Render thread to let a background compiler run "fairly", the player perceives stutter (1% low FPS drop) or input lag.

**The Hypothesis**: By removing fairness logic and optimizing the "Hot Path" to run faster than a DRAM access, we can achieve hardware-level responsiveness.

**The Solution**: A **7-Tier Priority System** combined with optimized scheduling primitives.

### Test Platform
- **CPU**: AMD Ryzen 7 9800X3D (8 cores, 16 threads with SMT) @ ~5.0 GHz
- **RAM**: 96GB DDR5
- **GPU**: NVIDIA RTX 4090
- **OS**: CachyOS 6.18 (Linux kernel 6.12+)

---

## 2. Architecture: The Zero-Cycle Engine

The core innovation of `scx_cake` is the reduction of scheduling overhead by **~70%** compared to standard implementations.

### Cycle Cost Analysis

#### Per-Function Breakdown

| Function | Role | Estimated Cycles | Key Optimizations |
|:---------|:-----|:-----------------|:------------------|
| `cake_enqueue` | Task wakeup | ~2-5c | Pre-computed tier (bitfield extract) |
| `cake_select_cpu` | Find idle CPU | ~20-30c | `scx_bpf_now()`, global bitmask, TZCNT |
| `cake_dispatch` | Move task to CPU | ~5-10c | Direct dispatch bypass |
| `cake_running` | Task starts | ~15-25c | `scx_bpf_now()`, scoreboard update |
| `cake_stopping` | Task stops | ~50-60c | Tier recalc, Kalman filter, next slice |
| `cake_update_idle` | CPU idle transition | ~5-10c | Single atomic OR |
| **Hot Path Total** | enqueue→select→dispatch | ~27-45c | -- |
| **Full Task Lifecycle** | All callbacks | ~80-140c | -- |

#### Call Frequency Estimation (Gaming Workload on 9800X3D)

| Event | Est. Frequency | Cycles/Event | Cycles/Second | % of 1 Core |
|:------|:---------------|:-------------|:--------------|:------------|
| Task wakes | ~100,000/sec | ~30c | 3,000,000 | 0.06% |
| Task stops | ~100,000/sec | ~55c | 5,500,000 | 0.11% |
| Idle transitions | ~50,000/sec | ~7c | 350,000 | 0.007% |
| **Total Scheduler Overhead** | -- | -- | **~9M cycles/sec** | **~0.18%** |

*Note: At 5 GHz, one core executes 5,000,000,000 cycles/sec. Scheduler overhead is <0.2% of a single core.*

#### Comparison: Before vs After Optimizations

| Metric | Naive Implementation | scx_cake | Improvement |
|:-------|:--------------------|:---------|:------------|
| Hot path (wakeup→dispatch) | ~400c | ~30c | **13x faster** |
| Full task lifecycle | ~470c | ~100c | **4.7x faster** |
| CPU idle check | ~300c (map scan) | ~5c (bitmask) | **60x faster** |
| Timestamp read | ~20c (TSC) | ~5c (cached) | **4x faster** |

---

### A. Pre-Computed Math (2 Cycles)
**The Innovation**: All complex math (Tier calculation, Slice adjustment) is moved to the "Stopping Path" (when a task finishes).

```c
/* OLD: Math calculated ON WAKEUP (Critical Path) - ~100 Cycles */
u64 vtime = (p->scx.dsq_vtime * 100) / weight; // Division!
u8 tier = calculate_tier(p->runtime);          // Branching!

/* NEW: Pre-Computed. Just Load. - 2 Cycles */
u8 tier = GET_TIER(tctx);  // L1 Load (bitfield extract)
```

### B. Global Bitmask Search (5 Cycles)
**The Innovation**: A Global `u64` Bitmask in `.bss` memory tracks idle CPUs.

```c
/* OLD: Map Scan - ~300 Cycles */
bpf_for(i, 0, 16) {
    if (is_idle(i)) return i;
}

/* NEW: Hardware Instruction (TZCNT) - 5 Cycles */
u64 mask = idle_mask;
if (mask) return __builtin_ctzll(mask);
```

### C. The Scoreboard (Neighbor Awareness)
**The Innovation**: A shared BPF Array (`cpu_tier_map`) where every CPU publishes its status for O(1) victim selection.

```c
/* Direct Array Read - 5 Cycles */
struct status *s = bpf_map_lookup_elem(&cpu_tier_map, &next_cpu);
if (s->tier >= INTERACTIVE) preempt(next_cpu);
```

### D. Direct Dispatch Bypass (5 Cycles)
**The Innovation**: Skip the kernel's dispatch queue for idle CPUs.

```c
/* OLD: Standard Queueing - ~50 Cycles */
scx_bpf_dispatch(p, SCX_DSQ_GLOBAL, slice);

/* NEW: Direct to CPU - 5 Cycles */
scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, SCX_ENQ_LAST);
```

---

## 3. The 7-Tier Priority System

Tasks are classified by **Behavior**, not "Niceness". The tier determines both the timeslice and how aggressively the task can preempt others.

| Tier | Name | Wait Budget | Starvation Limit | Slice Multiplier | Purpose |
|:-----|:-----|:------------|:-----------------|:-----------------|:--------|
| **0** | Critical Latency | 100µs | 5ms | 0.6x | Mouse/Keyboard input |
| **1** | Realtime | 750µs | 3ms | 0.8x | Audio, networking |
| **2** | Critical | 2ms | 4ms | 0.9x | Compositor, kernel threads |
| **3** | Gaming | 4ms | 8ms | 1.0x | **The Game Loop** |
| **4** | Interactive | 8ms | 16ms | 1.1x | Normal applications |
| **5** | Batch | 20ms | 40ms | 1.2x | Compilers, builds |
| **6** | Background | ∞ | 100ms | 1.3x | Encoders, bulk work |

### Tier Classification Logic
Tasks are classified based on their **sparse score** (0-100), computed from runtime behavior:
- Short, infrequent bursts → High score → Higher tier (more responsive)
- Long, continuous runs → Low score → Lower tier (background work)

```c
/* Branchless tier calculation using multiply-shift */
u32 bucket = (sparse_score * 3277) >> 16;  // Division by 20 without div instruction
u8 tier = 6 - clamp(bucket, 0, 6);
```

---

## 4. Performance Optimizations

This section documents the key performance concepts that make `scx_cake` fast. Each optimization targets a specific hardware or software bottleneck.

### Understanding CPU Cycles

Every operation in the scheduler costs CPU cycles. On a 5 GHz 9800X3D:
- **1 cycle** = 0.2 nanoseconds
- **100 cycles** = 20 nanoseconds
- Context switches cost **1000+ cycles**

The goal is to minimize cycles in the **hot path** (the code executed on every scheduling decision).

| Operation Type | Typical Cost | Example |
|:---------------|:-------------|:--------|
| Register operation | 1 cycle | `a + b` |
| L1 cache read | 3-4 cycles | Reading task context |
| L2 cache read | 12-15 cycles | Map lookup miss |
| L3 cache read | 30-50 cycles | Cross-core data |
| TSC read | 15-25 cycles | `bpf_ktime_get_ns()` |
| Integer division | 20-80 cycles | `x / 20` |
| Branch misprediction | 15-20 cycles | `if/else` wrong path |
| Atomic operation | 10-50 cycles | `__sync_fetch_and_or` |

---

### A. Timestamp Caching (`scx_bpf_now()`)

**The Problem**: Reading time is expensive.

`bpf_ktime_get_ns()` reads the hardware TSC (Time Stamp Counter), which requires:
1. A serializing instruction (stops pipeline)
2. Reading a hardware register
3. Cost: ~15-25 cycles per call

**The Solution**: Use `scx_bpf_now()` which reads the cached `rq->clock` that the kernel already maintains.

```c
/* OLD: Hardware TSC read - ~15-25 cycles */
u64 now = bpf_ktime_get_ns();

/* NEW: Cached rq->clock - ~3-5 cycles */
u64 now = scx_bpf_now();
```

**Impact**: ~40-80 cycles saved per task (4 call sites × ~10-20 cycles each).

---

### B. Cache Line Optimization

**The Problem**: False sharing destroys performance on multi-core systems.

When two CPUs update variables on the same 64-byte cache line:
1. CPU A writes → invalidates CPU B's cache copy
2. CPU B writes → invalidates CPU A's cache copy
3. This "ping-pong" can cost 50-100 cycles per access

**The Solution**: Isolate frequently-updated atomics on separate cache lines.

```c
u64 idle_mask SEC(".bss");        // Cache line 1
u64 __mask_pad[7] SEC(".bss");    // 56 bytes padding
u64 victim_mask SEC(".bss");      // Cache line 2 (separate!)
```

**Why 56 bytes?** 
- Cache line = 64 bytes
- `idle_mask` = 8 bytes
- Padding = 56 bytes
- Total = 64 bytes → `victim_mask` starts on new cache line

---

### C. Idle Path Streamlining

**The Problem**: Redundant operations on the idle path.

When a CPU goes idle, we were doing 3 operations:
1. Set bit in `idle_mask` (required)
2. Clear bit in `victim_mask` (redundant)
3. Update `cpu_tier_map` (redundant)

**The Analysis**:
- `idle_mask` is checked FIRST in `cake_select_cpu`
- If `idle_mask` says "idle", we dispatch there immediately
- Stale `victim_mask` data never gets used
- `cake_running` updates the scoreboard when a task actually starts

**The Solution**: Remove operations 2 and 3.

**Impact**: ~45-80 cycles saved per idle transition.

---

### D. Division-Free Arithmetic (Bitwise Magic)

**The Problem**: Integer division is slow.

On x86, division costs 20-80 cycles depending on operand size. In hot paths, this is unacceptable.

**The Solutions**:

#### Power-of-2 Division (Right Shift)
```c
/* Division by 1024 (close to 1000 for ns→µs conversion) */
u32 us = ns >> 10;  // 1 cycle vs 20+ cycles
```

#### Arbitrary Division (Multiply-Shift)
For non-power-of-2 divisors, we use "magic number" multiplication:

```c
/* Division by 20 for tier bucketing */
// Math: 3277/65536 ≈ 0.05004 ≈ 1/20
u32 bucket = (score * 3277) >> 16;
```

**How to calculate magic numbers**:
1. Choose shift amount (typically 16 or 32)
2. Magic = (2^shift) / divisor, rounded
3. For /20: magic = 65536/20 = 3276.8 ≈ 3277

| Divisor | Magic Number | Shift | Formula |
|:--------|:-------------|:------|:--------|
| 20 | 3277 | 16 | `x * 3277 >> 16` |
| 1000 | 1049 | 20 | `x * 1049 >> 20` |
| 1024 | -- | 10 | `x >> 10` (exact) |

---

### E. Branchless Programming

**The Problem**: Branch misprediction is expensive.

When the CPU predicts wrong:
1. Pipeline must be flushed (15-20 cycles wasted)
2. Correct path must be fetched and executed
3. For 50% branches, ~50% misprediction rate

**The Solution**: Replace branches with arithmetic.

#### The Signed Mask Trick
```c
/* OLD: Unpredictable branch */
if (condition) value = a; else value = b;

/* NEW: Branchless using signed mask */
s32 mask = -(s32)condition;  // 0 or 0xFFFFFFFF
value = (a & mask) | (b & ~mask);
```

**How it works**:
- `-(s32)1` = `0xFFFFFFFF` (all 1s)
- `-(s32)0` = `0x00000000` (all 0s)
- `a & 0xFFFFFFFF` = `a`
- `b & 0x00000000` = `0`

#### Conditional Increment (No Branch)
```c
/* OLD: Branch for increment */
if (flag) count++;

/* NEW: Branchless */
count += flag;  // flag is 0 or 1
```

---

### F. Pre-Computed Values

**The Problem**: Computing values at wake-up adds latency to the critical path.

**The Solution**: Move computation to the "cold path" (task stopping).

```c
/* In cake_stopping (cold path - happens AFTER task ran) */
new_tier = compute_tier_from_runtime(runtime);  // Complex math here
SET_TIER(tctx, new_tier);  // Store for later

/* In cake_enqueue (hot path - happens on WAKEUP) */
tier = GET_TIER(tctx);  // Just load pre-computed value (2 cycles)
```

**Impact**: ~100 cycles → ~2 cycles on wake-up path.

---

## 5. HPC Patterns & Industry Best Practices

The optimizations in `scx_cake` are informed by high-performance computing research:

### Confirmed Patterns (Already Implemented)

| Pattern | Source | Implementation |
|:--------|:-------|:---------------|
| **No Dynamic Allocation** | NASA JPL "Power of 10" | BPF enforces this by design |
| **Fixed Loop Bounds** | NASA JPL "Power of 10" | `for (i = 0; i < 64; i++)` with early break |
| **Division-Free Math** | Algorithmica HPC Book | `* 3277 >> 16` for `/20` |
| **Cache Line Awareness** | LMAX Disruptor | Padding between atomic masks |
| **Branchless Conditionals** | HPC Book | Signed mask trick |
| **O(1) Data Structures** | Flat-CG Pattern | Array indexing with `tier & 7` |
| **Lock-Free Atomics** | LMAX Disruptor | `__sync_fetch_and_*` operations |

### Evaluated But Not Implemented

| Pattern | Reason |
|:--------|:-------|
| Batch Dispatch | Risk of priority inversion for gaming tier |
| SMT-Aware CPU Selection | Unclear latency benefit, adds complexity |
| SIMD/Vectorization | BPF doesn't support; sequential task processing |
| Ring Buffers for Stats | Per-CPU arrays already optimal for counters |

### Key References
- **Algorithmica "Algorithms for Modern Hardware"**: Cache lines, branchless programming, integer division
- **NASA JPL "Power of 10" Rules**: Safety-critical C patterns for determinism
- **LMAX Disruptor**: Lock-free SPSC patterns, mechanical sympathy
- **Apollo Cyber RT**: Deterministic real-time scheduling for autonomous vehicles

---

## 6. Experimental Findings

### A. Performance Wins ✅

| Optimization | Cycle Savings | Impact | Date |
|:-------------|:--------------|:-------|:-----|
| **`scx_bpf_now()` replacement** | ~40-80c/task | Replaced 4 TSC reads with cached rq->clock | Dec 2025 |
| **Idle path streamlining** | ~45-80c/transition | Removed redundant victim_mask + scoreboard updates | Dec 2025 |
| **Cache line padding** | Variable | Eliminated false sharing between idle/victim masks | Dec 2025 |
| **Pre-computed tier math** | ~100c→2c | Moved tier calc from wakeup to stopping path | Initial |
| **Global bitmask (TZCNT)** | ~300c→5c | Replaced map scan with hardware instruction | Initial |
| **Direct dispatch bypass** | ~50c→5c | Skip kernel queue for idle CPUs | Initial |
| **Division-free arithmetic** | ~20c/div | Multiply-shift patterns instead of division | Initial |
| **Branchless conditionals** | ~5-15c each | Signed mask trick avoids branch misprediction | Initial |

### B. Performance Regressions & Lessons Learned ⚠️

| Experiment | What Went Wrong | Impact | Lesson |
|:-----------|:----------------|:-------|:-------|
| **1ms starvation limit** | Too aggressive for legitimate input bursts | 1% Low: 180→120 FPS | Allow variance; 5ms is the sweet spot |
| **Single DSQ architecture** | Runtime errors, DSQ ID conflicts | System instability | Tier-based DSQs provide necessary isolation |
| **100% direct dispatch** | Priority inversion when CPUs fully loaded | Latency spikes | Need fallback to tier DSQs for queuing |
| **Vtime fairness logic** | Added ~30 cycles for unwanted fairness | Gaming stutter | Deleted entirely; fairness is a non-goal |
| **Per-CPU cache density** | Packing hot data caused contention | ~10 FPS regression | Sometimes isolation > density |
| **Aggressive preemption** | Too many context switches | CPU overhead | Rate-limit preemption injection |

### C. The "1% Low" Regression (Case Study)
- **Experiment**: Tightened Critical Latency starvation limit to 1ms
- **Result**: 1% Low FPS dropped from 180 → 120
- **Analysis**: Legitimate heavy input frames occasionally take >1ms
- **Correction**: Relaxed to 5ms, restored performance
- **Lesson**: **Over-optimization kills stability.** The scheduler must allow for variance.

### D. The "FPS Overshoot" Phenomenon
- **Observation**: Games capped at 225 FPS often show 226-227 FPS
- **Cause**: Game engines calculate sleep time expecting scheduler latency
- **Significance**: Proves removal of software latency - CPU waits on GPU, not kernel

### E. Performance Baseline

| Metric | Before Optimization | After Optimization |
|:-------|:--------------------|:-------------------|
| Scheduler Latency | ~400 cycles | ~100 cycles |
| Average Wait Time | ~15µs | ~2µs |
| Direct Dispatch Rate | ~80% | ~91% |
| 1% Low FPS | 120 (regression) | **210+** |

---

## 7. Usage

### Quick Start
```bash
git clone https://github.com/RitzDaCat/scx_cake.git
cd scx_cake
./build.sh
sudo ./start.sh
```

### Monitoring (Verbose Mode)
```bash
sudo ./start.sh -v
```

In verbose mode, watch for:
- **Max Wait**: Should be <2ms for Gaming tier
- **StarvPreempt**: If high, system is actively suppressing heavy tasks
- **DirectDispatch%**: Should be >90%

### CLI Options
| Option | Default | Description |
|:-------|:--------|:------------|
| `--quantum` | 3ms | Base timeslice in milliseconds |
| `--new-flow-bonus` | 1.5ms | Bonus slice for newly woken tasks |
| `--sparse-threshold` | 125 | Threshold for sparse score calculation |
| `--starvation` | 5ms | Global starvation limit |
| `--verbose` / `-v` | off | Enable TUI monitoring |
| `--interval` | 1000ms | Stats update interval |

---

## 8. License & Acknowledgments

**License**: GPL-2.0

**Inspiration**:
- [CAKE (Common Applications Kept Enhanced)](https://www.bufferbloat.net/projects/codel/wiki/Cake/) - Queue management philosophy
- [sched_ext](https://github.com/sched-ext/scx) - BPF scheduler framework
- [Algorithmica HPC Book](https://en.algorithmica.org/hpc/) - Performance optimization patterns

**Test Hardware**: AMD Ryzen 7 9800X3D, 96GB DDR5, NVIDIA RTX 4090, CachyOS 6.18
