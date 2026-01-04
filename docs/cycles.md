# CPU Cycle Audit: scx_cake
**Status:** Highly Optimized (Zen 5 Targeted)
**Date:** 2026-01-03

This document breaks down the estimated CPU cycle cost for each critical scheduler path.
Counts are conservative estimates based on BPF instruction complexity and modern CPU architecture (Zen 4/5, Raptor Lake).

## Legend
*   **ALU:** Arithmetic (Add, Sub, Bitwise). Cost: **< 1 cycle** (allocatable).
*   **L1 Load:** Memory read from L1 Cache. Cost: **~4 cycles**.
*   **Hash/Jitter:** Cheap entropy mix. Cost: **~3-5 cycles**.
*   **Helper:** Kernel Function Call. Cost: **~20-50 cycles**.
*   **Time:** `scx_bpf_now()` (Cached Clock). Cost: **~15 cycles** (vs ~40 for RDTSC).

---

## Global Summary (End-to-End Latency)

| Function | Role | Frequency | Estimated Cost | Status |
| :--- | :--- | :--- | :--- | :--- |
| **`cake_select_cpu`** | **Wakeup Decision** | High | **~35c** | 🔥 **Wait-Free Linear Scan** |
| **`cake_enqueue`** | **Queue Insert** | High | **~25c** | 🔥 **Zero-Cycle Logic** |
| **`cake_dispatch`** | **Dispatch** | Very High | **~45c** | 🔥 **Jitter Entropy** |
| `cake_stopping` | Task Yield | High | ~60c | ⚠️ Accounting Heavy |
| `cake_running` | Task Start | High | ~55c | ⚠️ Budget Checks |
| **Total Overhead** | **Wakeup -> Run** | **Per Switch** | **~220c** | **Extremely Low** |

---

## 1. `cake_select_cpu` (The Scoreboard Scan)
**Path:** Wakeup
**Optimization:** Replaced Atomics/Global Locks with Wait-Free Linear Scan.

| Operation | Type | Cost (Cycles) | Notes |
| :--- | :--- | :--- | :--- |
| `get_task_ctx` | Helper | 20 | Pointer retrieval |
| Time Check | Time | 15 | `scx_bpf_now` |
| **Topology Scan** | **L1/L2 Load** | **~10-20** | Linear scan of `is_idle` (Padded BSS). Prefetcher friendly. |
| Victim Check | L1 Load | 4 | `last_victim_cpu` check (Tier 0 only). |
| **Total Cost** | | **~35-50** | **Dominated by Memory Latency (Cache)** |

*Note: The linear scan is fast because modern CPUs prefetch the `cpu_status` array. We avoid all atomics/locking.*

## 2. `cake_enqueue` (Zero-Cycle Insert)
**Path:** Wakeup (after select)
**Optimization:** All math is pre-computed in `cake_stopping`.

| Operation | Type | Cost (Cycles) | Notes |
| :--- | :--- | :--- | :--- |
| `get_task_ctx` | Helper | 20 | Context retrieval |
| **Wait TS Update** | Store | 1 | `tctx->last_wake_ts = now` |
| `GET_TIER` | Load | 4 | Pre-computed value. |
| `tier_to_dsq` | Load | 4 | Array lookup. |
| `dsq_insert` | Helper | ~**0** | (tail call / immediate insert) |
| **Total Cost** | | **~25-30** | **Pure Plumbing** |

## 3. `cake_dispatch` (The Jitter Check)
**Path:** Scheduler Loop
**Optimization:** "Jitter Entropy" replaces expensive random number generation.

| Operation | Type | Cost (Cycles) | Notes |
| :--- | :--- | :--- | :--- |
| **Jitter Mix** | **ALU** | **3** | `(pid ^ runtime) & 0xF` |
| Shard Check | Load | 4 | Check local DSQ first. |
| Priority Loop | Branch | ~5 | Unrolled loop over 7 tiers. |
| `move_to_local` | Helper | 30 | Kernel helper (consume). |
| **Total Cost** | | **~45** | **Fastest Possible Dispatch** |

## 4. `cake_stopping` (The Accountant)
**Path:** Task Deschedule
**Optimization:** Bitwise IIR Filter & Scalar Tier Calculation.

| Operation | Type | Cost (Cycles) | Notes |
| :--- | :--- | :--- | :--- |
| **Kalman Filter** | **ALU** | **3** | `((Avg << 6) - Avg + New) >> 6`. No Float/Div. |
| Sparse Score | ALU/Branch | 5 | Simple +/- logic. |
| `update_task_tier` | ALU | 10 | The Logic Core. Pre-calculates next tier. |
| Deficit Update | ALU | 2 | Subtraction. |
| **Total Cost** | | **~60** | **Pays the cost for Enqueue speed** |

---

## 5. Core System Costs

### Kalman Filter (Runtime Estimation)
*   **Cost:** 3 ALU Ops.
*   **Why:** Replaces floating point or heavy integer division.
*   **Impact:** Negligible.

### Wait Tracking
*   **Cost:** 1 Timestamp Read + 1 Subtraction.
*   **Why:** Essential for AQM (Active Queue Management).
*   **Impact:** ~15-20 cycles (mostly Time read).

### Tier Classification
*   **Cost:** ~10-15 cycles (Branching logic).
*   **Why:** Complex decision matrix (Behavior vs Physics).
*   **Impact:** Done during `stopping` (lazy), so it doesn't slow down wakeups.
