# scx_cake Optimization Strategy & Design Decisions

This document details the architectural decisions and optimization strategies that transform `scx_cake` from a standard BPF scheduler into a high-performance gaming engine. Our goal is **Zero-Overhead** and **Wait-Free** execution.

## 1. Architectural Philosophy

### A. "Unfairness is a Feature"
Standard schedulers (CFS, EEVDF) prioritize Fairness (Lag Compensation). In gaming, Fairness = Latency.
*   **Our Design:** We explicitly design for Unfairness.
*   **Tier 0 (Realtime):** Allowed to preempt anything.
*   **Tier 3 (Gaming):** Protected from background noise (Strict isolation).
*   **Result:** Input Latency drops to hardware limits (~2µs).

### B. "Zero-Cycle" Execution
We strive to move all math and logic **out of the hot path**.
*   **Hot Path:** `select_cpu`, `enqueue`, `dispatch` (Must be dumb data movers).
*   **Cold Path:** `stopping` (Do the math here when the task is done).

---

## 2. Data Structure Optimizations

### A. The Scoreboard (`cake_cpu_status`)
**Problem:** Tracking global CPU state usually requires atomic bitmasks (`idle_mask`) or global locks. This causes cache thrashing on high core counts (False Sharing).
**Solution:** A Distributed Wait-Free Array.
*   **Structure:** `struct cake_cpu_status cpu_status[CAKE_MAX_CPUS]` in .bss.
*   **Wait-Free Writer:** Each CPU writes *only* to its own index (`cpu_status[my_cpu]`). No locks needed.
*   **Cache Isolation:** Each struct is **padded to 64 bytes** (cache line size).
    *   *Result:* CPU 0 writing to its status never invalidates CPU 1's cache line.
    *   *Cost:* Storage space (negligible). *Gain:* Zero bus locking.

### B. Task Context Compression (`cake_task_ctx`)
**Problem:** Task context consumes memory and cache bandwidth.
**Solution:** Bit-packing and efficient layout.
*   **Packed Info:** We pack Score, Tier, Flags, and Wait Data into a single `u32` bitfield.
*   **Result:** The critical hot-path data fits into the first 32 bytes (half a cache line).

---

## 3. Critical Path Optimizations (Hot Path)

### A. The Linear Scan (Wait-Free `select_cpu`)
**Old Way:** Atomic Bitmask (`idle_mask`) + CTZ instruction.
*   *Issue:* Updates to the mask require atomic `LOCK XCHG` or `OR`, locking the memory bus.
**New Way:** Wait-Free Linear Scan of the Scoreboard.
*   We iterate through `scx_cake_scan_order[][]` (Topology Aware).
*   We simply **Load** `cpu_status[target].is_idle`.
*   **Why it's faster:**
    1.  No Atomic Instructions (which cost ~20+ cycles and stall pipelines).
    2.  Modern CPUs (Zen 4/5, Raptor Lake) have incredible hardware prefetchers that predict the linear memory access pattern.
    3.  Data is often already hot in L3 cache.

### B. Zero-Cycle Enqueue
**Problem:** When a task wakes up, calculating its `slice` and `tier` takes ~25-50 cycles of math.
**Solution:** Pre-Calculation in `cake_stopping`.
*   When a task *finishes* running, we know its runtime and behavior.
*   We calculate its *next* tier and slice right then.
*   We store these in `tctx->next_slice`.
*   **Wakeup:** `cake_enqueue` simply acts as a dumb loader: `insert(p, cached_tier, cached_slice)`.
*   *Cost during wakeup:* **~0 logic cycles**.

### C. Jitter Entropy (Dispatch)
**Problem:** Preventing starvation requires randomness (Probabilistic checking of low tiers). Generating random numbers in BPF is slow.
**Solution:** "Jitter Entropy".
*   We use the natural chaos of the system: `(pid ^ total_runtime_ns) & 0xF`.
*   `total_runtime_ns` captures interrupt jitter, cache misses, and variable execution time.
*   *Cost:* 3 ALU instructions. *Quality:* Sufficient for scheduling distribution.

---

## 4. Algorithmic Improvements

### A. HFT Kalman Filter
**Goal:** Estimate "Average Runtime" without storing a massive history window.
**Implementation:** Bitwise Exponential Moving Average (IIR Filter) inspired by High Frequency Trading.
*   Formula: `Avg = ((Avg << 6) - Avg + New) >> 6`.
*   Avoids all floating point and integer division.
*   Reacts quickly to behavior changes (Alpha = 1/64).

### B. Sticky Victim Strategy (Tier 0)
**Goal:** Prevent "Cache Trashing" when a light task (Mouse ISR) interrupts a heavy task (Game).
**Logic:**
*   When a Mouse Task preempts a Game Task on CPU 0...
*   The Mouse Task remembers: "I victimized CPU 0".
*   Next wakeup, it **sticks** to CPU 0 (if valid), running on top of the *same* victim.
*   **Benefit:** The Game Task stays hot in L1/L2 cache on CPU 0. The Mouse Task executes quickly and leaves.
*   **Avoids:** Migrating the interrupt to CPU 1, polluting CPU 1's cache, and forcing CPU 0's cache to go cold.

### C. Branchless Tier Selection
**Logic:** We avoid `if/else` chains in hot paths where possible.
*   Example: Wait Budget Lookup.
*   `budget = wait_budget[tier & 7];`
*   The array is padded to size 8. The mask ensures safety. The load is O(1) and branch-free.

---

## 5. Architectural Decisions

### A. Wait Tracking Fix (Jan 2026)
**Discovery:** Preempted tasks were not resetting their wait timers, leading to false demotions for CPU-bound apps.
**Fix:** Timestamp update moved to `cake_enqueue`.
*   Captures both Wakeups (Select -> Enqueue) and Preemptions (Running -> Enqueue).
*   Enables accurate Wait Budget logic for all workloads.

### B. Why FIFO DSQs?
We use `SCX_DSQ_LOCAL` (FIFO) instead of RB-Trees or Vtime.
*   **Reason:** Throughput. FIFO is O(1). RB-Tree is O(log N).
*   For a gaming scheduler, we serve the highest tier first. Sorting *within* a tier (Fairness) is unnecessary overhead.

### C. Why Wait-Free?
Wait-Free algorithms guarantee that no thread can block another.
*   In `scx_cake`, CPU 0 can update its state without ever waiting for CPU 1.
*   This removes the "Convoy Effect" where one slow core stalls the entire scheduler.
