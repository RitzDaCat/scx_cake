/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_cake BPF/userspace interface definitions
 *
 * Shared data structures and constants between BPF and Rust userspace.
 */

#ifndef __CAKE_INTF_H
#define __CAKE_INTF_H

#include <limits.h>

/*
 * Type definitions for BPF and userspace compatibility.
 * When vmlinux.h is included (BPF context), __VMLINUX_H__ is defined
 * and types come from there. Otherwise define them here.
 */
#ifndef __VMLINUX_H__
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long s64;
#endif

/*
 * Priority tiers with quantum multipliers (7-tier system)
 * * Higher tiers get SMALLER slices (more responsive)
 * Lower tiers get LARGER slices (better throughput)
 */
enum cake_tier {
    CAKE_TIER_CRITICAL_LATENCY = 0,  /* Ultra-low latency: score=100 AND <250µs avg runtime */
    CAKE_TIER_REALTIME    = 1,  /* Ultra-sparse: score=100 but >=250µs avg runtime */
    CAKE_TIER_CRITICAL    = 2,  /* Very sparse: audio, compositor */
    CAKE_TIER_GAMING      = 3,  /* Sparse/bursty: game threads, UI */
    CAKE_TIER_INTERACTIVE = 4,  /* Baseline: default applications */
    CAKE_TIER_BATCH       = 5,  /* Lower priority: nice > 0, heavy apps */
    CAKE_TIER_BACKGROUND  = 6,  /* Bulk work: compilers, encoders */
    CAKE_TIER_MAX         = 7,
};

/*
 * Flow state flags (only CAKE_FLOW_NEW currently used)
 */
enum cake_flow_flags {
    CAKE_FLOW_NEW = 1 << 0,  /* Task is newly created */
};

/*
 * Per-task flow state tracked in BPF (20 bytes used)
 * Fits 3 contexts per 64-byte cache line if packed tightly,
 * but padded here to 64B to prevent False Sharing.
 * * COMPRESSION:
 * - Timestamps: u32 (Wraps every 4.2s) - Acceptable for active gaming.
 * - Info: Packed into single u32 bitfield.
 */
struct cake_task_ctx {
    u64 next_slice;        /* 8B: OPTIMIZATION: Pre-computed slice (ns) */
    u32 last_run_at;       /* 4B: Last run (ns), wraps 4.2s */
    u32 packed_info;       /* 4B: Bitfield (Err, Score, Tier, Flags) */
    u16 deficit_us;        /* 2B: Deficit (us), max 65ms */
    u16 avg_runtime_us;    /* 2B: HFT Kalman Estimate */
    u8 __pad[44];          /* Pad to 64 bytes (Cache Line Size) */
};

/* * Bitfield Offsets for packed_info 
 * NOTE: Wait Data (Bits 8-15) is now unused/reserved hole.
 */
#define SHIFT_KALMAN_ERROR  0
/* Bits 8-15 Unused (formerly WAIT_DATA) */
#define SHIFT_SPARSE_COUNT  16
#define SHIFT_TIER          21
#define SHIFT_FLAGS         24
/* 28-31 Reserved */

#define MASK_KALMAN_ERROR   0xFF
/* MASK_WAIT_DATA Removed */
#define MASK_SPARSE_COUNT   0x1F  /* 5 bits: 0-31 */
#define MASK_TIER           0x07
#define MASK_FLAGS          0x0F

/* Delta scoring thresholds */
#define SPARSE_COUNT_INTERACTIVE 4   /* count >= 4 = Interactive tier */
#define SPARSE_COUNT_GAMING      8   /* count >= 8 = Gaming tier */
#define SPARSE_COUNT_CRITICAL   16   /* count >= 16 = Critical tier */
#define SPARSE_COUNT_REALTIME   24   /* count >= 24 = Realtime tier */
#define SPARSE_COUNT_MAX        31   /* count >= 31 = Critical Latency */

/*
 * Statistics shared with userspace
 */
struct cake_stats {
    u64 nr_new_flow_dispatches;    /* Tasks dispatched from new-flow */
    u64 nr_old_flow_dispatches;    /* Tasks dispatched from old-flow */
    u64 nr_tier_dispatches[CAKE_TIER_MAX]; /* Per-tier dispatch counts */
    u64 nr_sparse_promotions;      /* Sparse flow promotions */
    u64 nr_sparse_demotions;       /* Sparse flow demotions */
    /* * REMOVED: Wait stats (nr_wait_demotions, total_wait_ns, etc)
     * These were removed to optimize context switch performance.
     */
    u64 nr_starvation_preempts_tier[CAKE_TIER_MAX]; /* Per-tier starvation preempts */
    u64 nr_input_preempts;                 /* Preemptions injected for input/latency */
};

/*
 * Topology flags - set by userspace at load time
 * * These enable zero-cost specialization. When a flag is false,
 * the BPF verifier eliminates the corresponding code path entirely.
 * * Example: On 9800X3D (single CCD, no hybrid):
 * has_dual_ccd = false      → CCD selection code eliminated
 * has_hybrid_cores = false  → P-core preference code eliminated
 * Result: Zero overhead compared to no topology support
 */

/* Default values */
#define CAKE_DEFAULT_QUANTUM_NS         (2 * 1000 * 1000)   /* 2ms */
#define CAKE_DEFAULT_NEW_FLOW_BONUS_NS  (8 * 1000 * 1000)   /* 8ms */
#define CAKE_DEFAULT_SPARSE_THRESHOLD   50                  /* 5% = 50 permille */
#define CAKE_DEFAULT_INIT_COUNT         0                   /* Initial sparse count */
#define CAKE_DEFAULT_STARVATION_NS      (100 * 1000 * 1000) /* 100ms */

#endif /* __CAKE_INTF_H */