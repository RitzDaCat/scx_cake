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
 * 
 * Higher tiers get SMALLER slices (more responsive)
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
 * Per-task flow state tracked in BPF (16 bytes)
 * Fits exactly 4 contexts per 64-byte cache line (100% utilization).
 * 
 * COMPRESSION:
 * - Timestamps: u32 (Wraps every 4.2s) - Acceptable for active gaming.
 * - Info: Packed into single u32 bitfield.
 * - Slice: u32 (max 5.2ms fits in u32, saves 4 bytes).
 * - Runtime fields: Packed into single u32 (deficit:16, avg:16).
 */
struct cake_task_ctx {
    u32 next_slice;        /* 4B: Pre-computed slice (ns), reduced from u64 */
    u32 last_run_at;       /* 4B: Last run (ns), wraps 4.2s */
    u32 last_wake_ts;      /* 4B: Wake TS (ns), wraps 4.2s */
    u32 packed_info;       /* 4B: Bitfield (Err, Wait, Score, Tier, Flags) */
    /* Packed: deficit_us (lower 16 bits), avg_runtime_us (upper 16 bits) */
    u32 runtime_deficit;   /* 4B: deficit_us:16, avg_runtime_us:16 */
};

/* Bitfield Offsets for packed_info */
#define SHIFT_KALMAN_ERROR  0
#define SHIFT_WAIT_DATA     8
#define SHIFT_SPARSE_SCORE  16
#define SHIFT_TIER          23
#define SHIFT_FLAGS         26
/* 30-31 Reserved */

#define MASK_KALMAN_ERROR   0xFF
#define MASK_WAIT_DATA      0xFF
#define MASK_SPARSE_SCORE   0x7F
#define MASK_TIER           0x07
#define MASK_FLAGS          0x0F

/* Accessor macros for packed runtime_deficit field */
#define GET_DEFICIT_US(ctx)      ((u16)((ctx)->runtime_deficit & 0xFFFF))
#define SET_DEFICIT_US(ctx, val) ((ctx)->runtime_deficit = ((ctx)->runtime_deficit & 0xFFFF0000) | ((val) & 0xFFFF))

#define GET_AVG_RUNTIME_US(ctx)      ((u16)((ctx)->runtime_deficit >> 16))
#define SET_AVG_RUNTIME_US(ctx, val) ((ctx)->runtime_deficit = ((ctx)->runtime_deficit & 0xFFFF) | ((u32)(val) << 16))

/*
 * Statistics shared with userspace
 */
struct cake_stats {
    u64 nr_new_flow_dispatches;    /* Tasks dispatched from new-flow */
    u64 nr_old_flow_dispatches;    /* Tasks dispatched from old-flow */
    u64 nr_tier_dispatches[CAKE_TIER_MAX]; /* Per-tier dispatch counts */
    u64 nr_sparse_promotions;      /* Sparse flow promotions */
    u64 nr_sparse_demotions;       /* Sparse flow demotions */
    u64 nr_wait_demotions;         /* Tier demotions due to wait budget violation */
    u64 nr_wait_demotions_tier[CAKE_TIER_MAX]; /* Per-tier wait demotions (from which tier) */
    u64 nr_starvation_preempts_tier[CAKE_TIER_MAX]; /* Per-tier starvation preempts */
    u64 total_wait_ns;             /* Sum of all wait times */
    u64 nr_waits;                  /* Number of wait measurements */
    u64 max_wait_ns;               /* Maximum wait time seen */
    /* Per-tier wait time tracking */
    u64 total_wait_ns_tier[CAKE_TIER_MAX];  /* Sum of wait times per tier */
    u64 nr_waits_tier[CAKE_TIER_MAX];       /* Number of waits per tier */
    u64 max_wait_ns_tier[CAKE_TIER_MAX];    /* Max wait time per tier */
    u64 nr_input_preempts;                 /* Preemptions injected for input/latency */
};

/* Default values */
#define CAKE_DEFAULT_QUANTUM_NS         (4 * 1000 * 1000)   /* 4ms */
#define CAKE_DEFAULT_NEW_FLOW_BONUS_NS  (8 * 1000 * 1000)   /* 8ms */
#define CAKE_DEFAULT_SPARSE_THRESHOLD   100                  /* 10% = 100 permille */
#define CAKE_DEFAULT_STARVATION_NS      (100 * 1000 * 1000) /* 100ms */

#endif /* __CAKE_INTF_H */
