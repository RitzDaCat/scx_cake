# scx_cake Architecture Guide

> **Target Audience**: Engineers new to `sched_ext` or low-latency systems.
> **Goal**: To understand the code, design philosophy, and specific optimizations of the `scx_cake` scheduler.

---

## 1. High-Level Philosophy
`scx_cake` is a **Tier-Based, Latency-First** scheduler designed specifically for gaming and interactive workloads on Linux.

### The Problem it Solves
Standard Linux schedulers (EEVDF/CFS) prioritize **Fairness** and **Throughput**.
- They try to ensure every task gets its fair share of CPU time (Fairness).
- They try to minimize context switching to keep caches hot (Throughput).

For gaming, this is often wrong. One frame of a game involves:
1.  **Input**: Mouse moves (10µs).
2.  **Game Logic**: Main thread processes input (4ms).
3.  **Render**: Send commands to GPU (2ms).
4.  **Audio**: Mix sound buffer (500µs).

If the scheduler delays the **Input** by 2ms to be "Fair" to a background compiler, the frame feels laggy.
`scx_cake` prioritizes **Latency** (Time-to-Run) over Fairness.

---

## 2. Core Architecture: The "Tier" System
We classify every task into one of **7 Tiers** based on its behavior.

| Tier | Name | Target Workload | Criteria | Priority |
| :--- | :--- | :--- | :--- | :--- |
| **0** | **CritLatency** | Mouse, Keyboard, IRQs | Score=100, Avg < 50µs | **Highest** |
| **1** | **Realtime** | Network (Ping), Audio IO | Score=100, Avg < 500µs | Very High |
| **2** | **Critical** | Compositor (Wayland) | Score=90+ | High |
| **3** | **Gaming** | Game Main Loops, UI | Score=70+ | **Target** |
| **4** | **Interactive** | Browser, Discord, Tools | Score=50+ | Normal |
| **5** | **Batch** | Heavy Background Apps | Score=30+ | Low |
| **6** | **Background** | Compilers, Encoders | Score=0+ | Lowest |

### Dynamic Classification
We do NOT use static priorities (nice values). We analyze **behavior**.
- **The Score**: A value from 0-100 representing how "Sparse" (sleepy) a task is. Frequent sleepers = High Score.
- **The Avg Runtime**: An Exponential Moving Average (EMA) of how long the task runs when it wakes up.
- **The Logic**: If you sleep often and run briefly (Mouse), you are Tier 0. If you run forever (Compiler), you drop to Tier 6.

---

## 3. Data Structures (The "State")
The scheduler state is kept in `src/bpf/intf.h`.

### `struct cake_task_ctx` (Per-Task)
The "Backpack" carried by every task.
- **`packed_info`**: A single `u32` containing Tier, Score, and Flags.
    - *Why?* To fit everything into one CPU register for bitwise math.
- **`avg_runtime_us`**: The historic runtime average (used for Tier 0/1 detection).
- **`last_wake_ts`**: When the task woke up (used to calculate Wait Time).
- **`__pad`**: Padding to 64 bytes.
    - *Why?* **Cache Coherency**. We ensure each task's data has its own cache line so CPUs don't fight over ownership.

### `struct cake_cpu_status` (The Scoreboard)
A global array enabling CPUs to see what others are doing without locking.
- **`is_idle`**: Is this CPU free?
- **`tier`**: What Tier is running here?
- **Wait-Free Design**: Each CPU writes *only* to `cpu_status[my_cpu]`. All other CPUs only *read*. This eliminates the need for Spinlocks or Atomics.

---

## 4. Key Functions (The Life Cycle)

### A. `cake_select_cpu` (The Brain)
Called when a task wakes up. "Where should I run?"
1.  **Sticky Check**: Is the CPU I ran on last time (`prev_cpu`) idle? Use it (Cache Locality).
2.  **Idle Scan**: Scan all CPUs to find an idle one.
    - *Optimization*: **Topology-Aware**. We scan neighbors first (siblings/LLC) using a circular bitmask loop.
3.  **Preemption (The "Kick")**:
    - If I am **Tier 0** (Critical) and the system is full:
    - Find a CPU running a low-priority task (Tier 1-6).
    - **Hybrid Preemption**:
        1.  **Lazy**: Set `victim->slice = 0`. (Ensures they yield in ~1ms).
        2.  **Active**: If 75µs passed since last kick, call `scx_bpf_kick_cpu()`. (Immediate ~2µs yield).
    - *Why Hybrid?* Pure Kick is too expensive (CPU overhead). Pure Lazy is too slow (Input jitter). 75µs limit is the "Golden Mean".

### B. `cake_enqueue` (The Waiting Room)
Called when a task is ready to run but no CPU was found.
1.  **Stats**: Record the enqueue event.
2.  **Sharding**: If the task is Tier 3 or 4, we hash it to a "Shard" (DSQ sub-queue).
    - *Why?* A single global queue causes lock contention. Sharding splits the line into 4 smaller lines.
3.  **Insert**: Put the task into the Dispatch Queue (DSQ) for its Tier.

### C. `cake_dispatch` (The Picker)
Called when a CPU is empty and needs work.
1.  **Starvation Logic**: Occasionally (pseudo-randomly), check the lowest Tier (6) even if high Tiers have work.
    - *Why?* Prevents the compiler from hanging 100% while you game.
2.  **Priority Loop**: Check Tier 0 -> Tier 1 -> ... -> Tier 6.
3.  **Stealing**: If my local Shard is empty, check other Shards.

### D. `cake_stopping` (The Accountant)
Called when a task stops running.
1.  **Math**: Calculate how long it ran (`runtime = now - last_run`).
2.  **Update**: Update the `avg_runtime_us` (EMA) and `score`.
3.  **Re-Tier**: Recalculate the Tier for next time.
    - *Optimization*: We do this *after* running, so `enqueue` (the critical path) is strictly O(1) load-only.

---

## 5. Specific Optimizations

### 75µs Hybrid Cooldown
We rate-limit hardware interrupts ("Kicks") to one every 75µs per core.
- **Physics**: 8000Hz Mice poll every 125µs.
- **Safety**: 75µs is faster than the mouse (no jitter) but limits CPU overhead to ~2.6% worst-case.

### Branchless Math
We avoid `if/else` where possible.
- *Example*: `tier -= (adj & mask)`.
- *Why?* Keeps the CPU Instruction Pipeline full. Branches cause pipeline flushes (~15-20 cycle penalty).

### BSS Arrays vs Maps
We use C-style global arrays (`static struct ...`) instead of BPF Maps.
- *Why?* Arrays are direct memory addresses (Load/Store). Maps involve function calls limits (`bpf_map_lookup_elem`) and hashing overhead.

### Topology Awareness
We know which CPUs are neighbors (Cache Siblings).
- We scan neighbors first in `select_cpu`.
- We steal from neighbors first in `dispatch`.
- *Result*: Migrating a task to a neighbor is cheap (L3 Cache hit). Migrating to a distant core is expensive (RAM fetch).

---

## 6. How to Verify
1.  **`vmstat 1`**: Watch `in` (Interrupts) and `cs` (Context Switches).
    - High `in` (>10k) = Heavy Preemption active (Good for latency).
    - Massive `cs` (>50k) = Thrashing (Bad).
2.  **`scx_cake_stats`**: Run with `--verbose`.
    - Check `nr_input_preempts`: Is the Kick working?
    - Check `nr_wait_demotions`: Are we starving low tiers?

---

> This scheduler is a living machine tailored for **Responsiveness**. It assumes that *waiting* is the enemy, and we spend CPU cycles (via Kicks and Checks) to buy Time.
