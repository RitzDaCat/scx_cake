# scx_cake Scheduler Design & Logic Flow

This document provides a comprehensive breakdown of the `scx_cake` BPF scheduler code, categorized by execution path, function responsibility, and core algorithmic systems.

## Legend: Execution Paths

*   🔥 **HOT PATH:** Critical performance path. Executed for every task wakeup, switch, or migration. Must be extremely fast (nanoseconds).
*   ⚠️ **WARM PATH:** Executed frequently (e.g., on timer ticks, once per ms) but not on every single event.
*   🧊 **COLD PATH:** Executed only once (startup/shutdown) or rarely (task creation/exit).

## Function Breakdown

| Function Name | Path | Type | Primary Responsibility | Key Logic Flow |
| :--- | :---: | :--- | :--- | :--- |
| **`cake_select_cpu`** | 🔥 | BPF Ops | **CPU Selection**<br>Decides where a waking task runs. | 1. **Wake Sync:** If `SCX_WAKE_SYNC`, run on current CPU (Hot Handoff).<br>2. **Prev CPU:** Check if previous CPU is idle.<br>3. **Sticky Victim:** (Tier 0 only) Return to last preempted victim.<br>4. **Topology Scan:** Linear scan of `scan_order` (L3-aware) for idle CPUs.<br>5. **Preempt Injection:** (Tier 0 only) Kick victim if no idle found. |
| **`cake_enqueue`** | 🔥 | BPF Ops | **Task Queuing**<br>Places task into a DSQ. | 1. **Get Context:** Retrieve/Init `cake_task_ctx`.<br>2. **Update Wait TS:** Set `last_wake_ts` (Critical for accurate wait tracking).<br>3. **Get Tier:** Read pre-calced tier.<br>4. **Select DSQ:** Hash PID+CPU to pick Shard 0 or 1.<br>5. **Insert:** `scx_bpf_dsq_insert` with pre-calced time slice. |
| **`cake_dispatch`** | 🔥 | BPF Ops | **Task Picking**<br>CPU asks "What do I run next?" | 1. **Anti-Starvation:** Randomly check low tiers (Jitter Entropy).<br>2. **Priority Loop:** Iterate Tiers 0 → 6.<br>3. **Local Shard:** Check local shard first (Affinity).<br>4. **Remote Shard:** Check other shard (Work Stealing).<br>5. **Consumer:** `scx_bpf_dsq_move_to_local`. |
| **`cake_stopping`** | 🔥 | BPF Ops | **Task Yield/Preempt**<br>Task is leaving the CPU. | 1. **Calc Runtime:** `now - last_run_at`.<br>2. **Kalman Filter:** Update `avg_runtime_us` (EMA).<br>3. **Sparse Score:** Update behavior score (Interactive vs Batch).<br>4. **Deficit:** Charge runtime against deficit.<br>5. **Pre-Calc:** Call `update_task_tier` for *next* wakeup (0-cycle enqueue). |
| **`cake_running`** | 🔥 | BPF Ops | **Task Start**<br>Task is entering the CPU. | 1. **Scoreboard:** Update global `cpu_status` (Wait-Free).<br>2. **Wait Budget:** Check `now - last_wake_ts` vs Tier Budget.<br>3. **Demotion:** If violations > 30%, penalize score (drop tier).<br>4. **Stats:** Update global/tier statistics. |
| **`cake_update_idle`** | 🔥 | BPF Ops | **Idle Tracking**<br>CPU going to sleep/waking. | 1. **Scoreboard:** Set `cpu_status[cpu].is_idle` to 1 or 0.<br>2. Used by `select_cpu` for fast idle finding. |
| **`cake_tick`** | ⚠️ | BPF Ops | **Starvation Check**<br>Periodic (Hz) check. | 1. **Check Runtime:** `now - last_run_at`.<br>2. **Threshold:** Compare vs Tier Starvation Limit (e.g. 8ms for Game).<br>3. **Kick:** If exceeded, `scx_bpf_kick_cpu` (Force Preemption). |
| **`cake_init`** | 🧊 | BPF Ops | **Initialization**<br>Scheduler startup. | 1. **Pre-calc:** Constants (thresholds).<br>2. **Scoreboard:** Init `cpu_status` for all CPUs.<br>3. **Create DSQs:** Create 14 Queues (7 Tiers * 2 Shards). |
| **`cake_exit`** | 🧊 | BPF Ops | **Teardown** | 1. Report Exit Info (`UEI`). |
| **`cake_enable`** | 🧊 | BPF Ops | **Task Join** | 1. No-op (Context created lazily on first access). |
| **`cake_disable`** | 🧊 | BPF Ops | **Task Leave** | 1. **Cleanup:** `bpf_task_storage_delete` (Free memory). |

## Core Systems & Algorithms

### 1. Runtime Detection (The Kalman Filter)
**Goal:** Smoothly track how "heavy" a task is to distinguish between interactive tasks (short bursts) and batch tasks (long compute).
*   **Mechanism:** Exponential Moving Average (EMA) IIR Filter.
*   **Implementation:** Bitwise math optimization without division.
*   **Loc:** `update_kalman_estimate`
*   **Filtering:** Alpha = 1/64 (Samples ~16ms history at 8KHz polling).
*   **Formula:** `Avg = ((Avg << 6) - Avg + New) >> 6`

### 2. Wait Tracking (Wait Budget AQM)
**Goal:** Detect when tasks are getting stuck in queues and demote "imposters" (tasks posing as high tier but causing congestion).
*   **Start:** Timer starts in `cake_enqueue` (or `select_cpu` direct dispatch). `tctx->last_wake_ts = now`.
*   **End:** Timer stops in `cake_running`. `wait_time = now - last_wake_ts`.
*   **Check:** Compares `wait_time` against `wait_budget[tier]`.
*   **Penalty:** 
    *   Uses a 10-sample sliding window (probabilistic).
    *   If **> 30%** (3 out of 10) runs violate the budget, the task's "Sparse Score" is penalized (-10 points).
    *   This forces the task to drop to a lower priority tier to find its "natural" level.

### 3. Tier Classification (The Sorting Hat)
**Goal:** Assign tasks to one of 7 priority tiers (0=Realtime, 6=Background) based on behavior.
*   **Inputs:** 
    *   **Behavior:** Sparse Score (0-100). Higher = more interactive/yielding.
    *   **Physics:** Average Runtime (µs).
*   **Logic:** `Tier = MAX(Score_Tier, Runtime_Tier)`
*   **Philosophy:** "You must be THIS sparse AND THIS fast to handle this ride."
*   **Example:** A task with Score 100 (Perfect) but Runtime 5ms (Slow) cannot be Tier 0 (Realtime). It will be forced to Tier 3 (Game) or Tier 4.

### 4. Starvation Protection
**Goal:** Ensure low-priority tasks (Tier 6) eventually run, even if the system is overloaded with higher tier work.
*   **Soft (Dispatch Level):** "Jitter Entropy".
    *   In `cake_dispatch`, we check `(pid ^ total_runtime) & 0xF == 0`.
    *   roughly 1 in 16 times, we explicitly check the lowest tier (DSQ 6) before checking high tiers.
    *   Uses natural system jitter (runtime nanoseconds) as a random source.
*   **Hard (Tick Level):** Preemption "Kick".
    *   In `cake_tick`, we check `runtime > starvation_threshold[tier]`.
    *   If a task hogs the CPU longer than allowed (e.g. >8ms for Game Tier), we fire `scx_bpf_kick_cpu` to force a context switch.

### 5. Sticky Victim (Tier 0 Only)
**Goal:** Minimise cache trashing when "Light" tasks (like mouse interrupts) interrupt "Heavy" tasks (like a Game).
*   **Logic:**
    *   When a Tier 0 task wakes up, it checks `tctx->last_victim_cpu`.
    *   It tries to run *on top of the same victim it preempted last time*.
    *   This keeps the Game loop on CPU 0 and the Mouse Interrupt on CPU 0 (temporarily), leaving CPUs 1-15 undisturbed.
    *   **Effect:** Prevents the "interrupt storm" from migrating across all cores and flushing their L1/L2 caches.
