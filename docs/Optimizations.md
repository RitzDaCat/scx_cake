# scx_cake Optimization Journal

This document records the hardware-level optimizations and architectural experiments applied to `scx_cake` to maximize gaming performance and minimize latency.

## 1. Memory Layout & Cache Density

### Double Density Struct Packing (32 Bytes)
**Status:** ✅ Implemented
**Goal:** Maximize L1/L2 cache efficiency by fitting multiple task contexts into a single cache line.
**Implementation:**
- Compressed `cake_task_ctx` from 56 bytes to **32 bytes**.
- **EWMA vs Memory:** Replaced `total_runtime` (8B) and `run_count` (4B) with a single `avg_runtime_us` (4B) using Exponential Weighted Moving Average. This saves memory while providing a more adaptive metric.
- **Bit-Packing:** Compressed `wait_violations` and `wait_checks` from `u16` to 4-bit nibbles packed into a single `u8`.
- **Downsizing:** Reduced `sparse_score` from `u32` to `u8` (range 0-100).
**Result:**
- A standard CPU cache line is 64 bytes.
- Old layout (56B): Fitted 1 task per line (wasting 8B padding).
- New layout (32B): Fits **2 tasks per line** exactly.
- **Impact:** Effectively doubled the L2 cache capacity for scheduler contexts, reducing cache pollution during high-frequency iterating.

### [FAILED] Cache Line Isolation (Padding)
**Status:** ❌ Reverted
**Experiment:**
- We attempted to pad `cake_task_ctx` to 128 bytes to isolate "Read-Mostly" fields (tier, flags) from "Write-Heavy" fields (runtime, deficit) onto separate physical cache lines to prevent False Sharing.
**Result:**
- **Regression:** Average FPS dropped by 20, and 1% low frametimes dropped from ~180 to ~120 (-33%).
- **Diagnosis:** The increased memory footprint (128B vs 56B) caused L2 cache thrashing. The penalty of fetching more cache lines outweighed the benefit of reduced coherence traffic.
**Learning:** For this scheduler architecture, **Cache Density > Cache Isolation**.

## 2. Hardware Concurrency

### Lockless Per-CPU Statistics
**Status:** ✅ Implemented
**Goal:** Eliminate memory bus contention caused by atomic instructions in the hot path.
**Problem:**
- Global statistics used `__sync_fetch_and_add` (compiles to `LOCK XADD`).
- This instruction locks the CPU memory bus/coherency fabric, causing stalls across ALL cores even if they are unrelated to the current task.
**Implementation:**
- Replaced global variables with `BPF_MAP_TYPE_PERCPU_ARRAY`.
- Each CPU core updates its own private, local memory bucket (`cnt++`).
- 0 bus locking. 0 cross-core communication during runtime.
- Aggregation is performed lazily in userspace (Rust) only when printing the details.
**Impact:** Eliminates micro-stutter caused by statistics collection.

## 3. Arithmetic Optimizations

### Fixed-Point Slice Calculation
**Status:** ✅ Implemented
- Replaced percentage-based integer division (`slice * multiplier / 100`) with bitwise shifts (`slice * multiplier >> 10`) using base-1024 fixed-point math.
- **Benefit:** Division is slow (20-80 cycles). Shifts are fast (1 cycle).

### Bitwise Vtime Scaling
**Status:** ✅ Implemented
- Pre-calculated an inverse weight lookup table for all distinct `nice` values.
- Replaced `delta / weight` division with `(delta * inverse_weight) >> 16`.
- **Benefit:** O(1) complexity for virtual time accounting.

### Branchless Score Updates
**Status:** ✅ Implemented
- Replaced `if (runtime < threshold)` branching logic with arithmetic logic.
- **Benefit:** Mitigates branch misprediction penalties in the scheduler's most frequent code path.


## 4. Starvation & Tier Tuning (Esports Strategy)

### "Strict Entry, Generous Exit"
**Status:** ✅ Implemented
**Goal:** Resolve the paradox where high-performance input tasks (Avg 30µs) occasionally spike (800µs) and get demoted/stuttered by strict safety nets.

**Experiment 1: Strict Limits (200µs)**
*   **Result:** High "Starvation Preempts" (~30k/session).
*   **Impact:** Any frame processing taking >200µs was killed instantly, causing micro-stutters and 5ms+ latency spikes (waiting for next slice).

**Experiment 2: Expanded Preemption (Score >= 90)**
*   **Idea:** Allow tasks with Score 90-99 (Critical) to trigger "Input Preemption" to eliminate wait times.
*   **Result:** **Regression.** 1% Low FPS dropped by 60 frames.
*   **Diagnosis:** "Noisy Neighbor" problem. Semi-active apps (Discord, Browser) with Score 94 started interrupting the main Game Thread.

**Golden Configuration:**
*   **Strict Entry:** Only \`Score == 100\` tasks qualify for \`CritLatency\` (VIP Tier).
*   **Generous Exit:** \`CritLatency\` Starvation Limit increased to **5ms (5000µs)**.
*   **Reasoning:** We filter aggressively at the door (Score 100), but once a task is trusted, we give it ample room to finish work without interruption.
*   **Result:** 1% Low FPS recovered to ~180. StarvPreempts dropped to near zero.

## 5. Struct Layout & Density

### Current Layout (32 Bytes)
**Status:** ✅ Verified
**Organization:** Optimized for Alignment (Largest to Smallest)
1.  **Offsets 0-23 (24B):** `last_run_at`, `last_wake_at`, `deficit` (3x u64).
2.  **Offsets 24-25 (2B):** `avg_runtime_us` (u16).
3.  **Offsets 26-31 (6B):** `kalman_error`, `padding`, `sparse_score`, `tier`, `flags`, `wait_data` (6x u8).
**Result:** Fits exactly 2 tasks per 64-byte cache line.

### Feasibility of 16 Bytes
**Status:** ⚠️ Theoretical / Risky
To reach 16 bytes (4 tasks/line), we must shrink the 24 bytes of timestamps.
*   **Option:** Convert `u64` -> `u32` (Wrap every 4s) AND Delete `last_wake_at`.
*   **Tradeoff:** Background tasks (>4s sleep) lose fairness.
