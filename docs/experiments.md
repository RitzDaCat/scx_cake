# Scheduler Optimization Experiments: The Quest for Efficiency

This document tracks our iterative journey to optimize `scx_cake` for high IPC (Parallelism) and low RES Interrupts (Consolidation), using EEVDF as the baseline.

## The Goal

- **IPC**: > 1.0 (Parallelism / Throughput)
- **RES Interrupts**: < 100 (Consolidation / Silence)
- **Latency**: Low for Gaming tasks.

## Experiment Log

| Strategy                   | Key Logic                                       | IPC      | RES / sec | Verdict                                                                      |
| :------------------------- | :---------------------------------------------- | :------- | :-------- | :--------------------------------------------------------------------------- |
| **Baseline (EEVDF)**       | CFS/EEVDF Defaults                              | **0.85** | **~15**   | **The Gold Standard.**                                                       |
| **Original scx_cake**      | Complex Idle Hunt + Preemption Injection        | 0.95     | ~1600     | **IPI Storm.** Too aggressive.                                               |
| **Simplified**             | Sync Wake + Sticky Pref                         | 0.51     | ~1450     | **Regression.**                                                              |
| **Aggressive Local**       | Always `current_cpu` (No Idle Hunt)             | 0.50     | **~4**    | **Serialized.**                                                              |
| **Hybrid**                 | Sync Wake + Global Idle Hunt                    | **1.00** | ~1300     | **Paralellism Restored.** Proof that Idle Hunting fixes IPC.                 |
| **Consolidator**           | Sticky Only (No Idle Hunt)                      | 0.55     | ~260      | **IPC Crash.** Starved CPUs.                                                 |
| **Reluctant**              | Queue Depth > 0 Check                           | 0.62     | ~350      | **Too Slow.** Migration happens too late.                                    |
| **Delegation**             | Kernel Fallback (`select_cpu_dfl`)              | 0.56     | ~800      | **Failure.** Kernel SIS is conservative under BPF.                           |
| **SMT-Aware**              | Hunt SMT Sibling -> Global Fallback             | 0.52     | ~700      | **Failure.** Prioritizing SMT didn't unblock the pipeline sufficienty.       |
| **Refined Hybrid**         | SMT + 50us Rate Limit                           | 0.55     | ~950      | **Failure.** Rate Limit killed parallelism. Back to start.                   |
| **Tiered Hybrid v1**       | Gaming: Hunt Always, BG: Sticky                 | **1.02** | **1092**  | **SUCCESS!** IPC restored (+85%), RES acceptable. Rate limit removed.        |
| **Phase 1 Opt** (Jan 2026) | Removed 9 redundant `dsq_nr_queued` checks      | **1.25** | ~1540     | **+21% IPC!** Dispatch optimization improved gaming perf significantly.      |
| **Phase 2 Opt** (Jan 2026) | Early `mask==0` return + single timestamp write | **0.95** | ~1460     | **REGRESSION (-24%).** Early return broke idle hunting logic. REVERT NEEDED. |

## The Final Pivot: "Unshackled Gaming"

**The Pattern**:

- Any restriction on migration (Sticky, Reluctant, Rate Limit) drops IPC to ~0.50.
- Only **Unrestricted Hunting** (Hybrid) hits 1.0 IPC.

**Conclusion**:
To match EEVDF's 0.85-1.3 IPC, we must migrate eagerly.
RES counts of ~1000/s are likely unavoidable for a BPF scheduler without internal load-tracking hooks.
However, we can minimize the _impact_ of those interrupts.

**New Plan: "Tiered Hybrid"** ✅ IMPLEMENTED (Jan 2026)

Most tasks (Background/Batch) don't need IPC 1.0. They need efficiency.
Gaming tasks need IPC 1.0 (Latency).

**Logic**:

1.  **Gaming/Critical (Tiers 0-3)**: **NO LIMITS**. Idle Hunt Always. (Target IPC 1.0).
2.  **Background/Batch (Tiers 4-6)**: **STICKY**. Consolidate on prev_cpu (Low RES).

**Implementation**: Removed 50µs rate limit. Gaming tiers hunt unrestricted, background tiers prefer sticky.

This is the **Tiered Split** refined with SMT awareness:

- Gaming: Hunt SMT -> Hunt Global.
- Background: Stick to Prev.

**Expected Results**: IPC 1.00+ for gaming workloads, low RES for background tasks.
