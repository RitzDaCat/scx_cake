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
    CAKE_TIER_0 = 0,  /* Tier 0: score=100 AND <250µs avg runtime */
    CAKE_TIER_1 = 1,  /* Tier 1: score=100 but >=250µs avg runtime */
    CAKE_TIER_2 = 2,  /* Tier 2: audio, compositor */
    CAKE_TIER_3 = 3,  /* Tier 3: game threads, UI */
    CAKE_TIER_4 = 4,  /* Tier 4: default applications */
    CAKE_TIER_5 = 5,  /* Tier 5: nice > 0, heavy apps */
    CAKE_TIER_6 = 6,  /* Tier 6: compilers, encoders */
    CAKE_TIER_MAX = 7,
};

/*
 * DSQ Sharding
 * Number of shards per tier (all tiers use equal sharding).
 * Must be a power of 2 for fast masking.
 */
#define SCX_DSQ_SHARD_COUNT 2
#define SCX_DSQ_SHARD_MASK  (SCX_DSQ_SHARD_COUNT - 1)

/*
 * Flow state flags (only CAKE_FLOW_NEW currently used)
 */
enum cake_flow_flags {
    CAKE_FLOW_NEW = 1 << 0,  /* Task is newly created */
};

/*
 * Per-task flow state tracked in BPF (24 bytes)
 * Fits 2-3 contexts per 64-byte cache line.
 * 
 * COMPRESSION:
 * - Timestamps: u32 (Wraps every 4.2s) - Acceptable for active gaming.
 * - Info: Packed into single u32 bitfield.
 */
struct cake_task_ctx {
    u64 next_slice;        /* 8B: OPTIMIZATION: Pre-computed slice (ns) */
    u64 last_run_at;       /* 8B: Last run (ns), no wrap */
    u64 last_wake_ts;      /* 8B: NEW: Wake TS (ns), no wrap */
    u32 packed_info;       /* 4B: Bitfield (Err, Wait, Score, Tier, Flags) */
    u16 deficit_us;        /* 2B: NEW: Deficit (us), max 65ms */
    u16 avg_runtime_us;    /* 2B: HFT Kalman Estimate */
    s32 last_victim_cpu;   /* 4B: NEW: Sticky Victim (The Bully Strategy) */
    u8 preferred_l3;       /* 1B: Preferred L3/CCX for affinity (255 = unset) */
    u8 __pad[27];          /* 28 -> 27 bytes (Total 64) */
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

/*
 * The Scoreboard: Per-CPU Status
 * Replaces global atomic masks with a distributed Wait-Free array.
 * Each CPU writes ONLY to its own slot.
 * Readers scan the array linearly.
 */
#define CAKE_MAX_CPUS 64

struct cake_cpu_status {
    u8 is_idle;            /* 1B: 1 if idle, 0 if busy */
    u8 tier;               /* 1B: Current running tier */
    u8 smt_sibling;        /* 1B: SMT sibling CPU ID (255 = none) */
    u8 l3_id;              /* 1B: L3 cache / CCX ID */
    u8 __pad_0[4];         /* Align u64 */
    u64 started_at;        /* 8B: NEW: Timestamp of current slice start */
    u8 __pad[48];          /* 62 -> 48 bytes (Total 64) */
};

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
