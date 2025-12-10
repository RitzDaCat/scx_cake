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
 * Priority tiers (like CAKE's DiffServ tins)
 */
enum cake_tier {
    CAKE_TIER_VOICE     = 0,  /* Audio, input events - lowest latency */
    CAKE_TIER_GAMING    = 1,  /* Interactive/gaming - low latency */
    CAKE_TIER_BESTEFFORT = 2, /* Normal applications - fair share */
    CAKE_TIER_BACKGROUND = 3, /* Bulk work - lowest priority */
    CAKE_TIER_MAX       = 4,
};

/*
 * Flow state flags
 */
enum cake_flow_flags {
    CAKE_FLOW_NEW       = 1 << 0,  /* Task is in new-flow list */
    CAKE_FLOW_SPARSE    = 1 << 1,  /* Task is sparse (low CPU usage) */
    CAKE_FLOW_BOOSTED   = 1 << 2,  /* Manually boosted (e.g., gaming PID) */
};

/*
 * Per-task flow state tracked in BPF
 */
struct cake_task_ctx {
    u64 deficit;           /* Time owed to this task (ns) */
    u64 last_run_at;       /* Last time task ran (ns) */
    u64 total_runtime;     /* Total accumulated runtime (ns) */
    u64 last_wake_at;      /* Last wakeup time (ns) */
    u32 wake_count;        /* Number of wakeups (for sparse detection) */
    u32 run_count;         /* Number of times scheduled */
    u32 sparse_score;      /* 0-100, higher = more sparse */
    u8  tier;              /* Priority tier */
    u8  flags;             /* Flow flags */
    u8  _pad[2];           /* Padding for alignment */
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
    u64 total_wait_ns;             /* Sum of all wait times */
    u64 nr_waits;                  /* Number of wait measurements */
    u64 max_wait_ns;               /* Maximum wait time seen */
};

/*
 * Configuration passed from userspace to BPF
 */
struct cake_config {
    u64 quantum_ns;        /* Base scheduling quantum (ns) */
    u64 new_flow_bonus_ns; /* Extra time for new flows (ns) */
    u64 sparse_threshold;  /* CPU usage threshold for sparse (permille, 0-1000) */
    u64 starvation_ns;     /* Maximum time before forcing dispatch */
};

/* Default values */
#define CAKE_DEFAULT_QUANTUM_NS         (4 * 1000 * 1000)   /* 4ms */
#define CAKE_DEFAULT_NEW_FLOW_BONUS_NS  (8 * 1000 * 1000)   /* 8ms */
#define CAKE_DEFAULT_SPARSE_THRESHOLD   100                  /* 10% = 100 permille */
#define CAKE_DEFAULT_STARVATION_NS      (100 * 1000 * 1000) /* 100ms */

/* DSQ IDs - per tier, with new/old flow variants */
#define CAKE_DSQ_NEW_BASE   0x100
#define CAKE_DSQ_OLD_BASE   0x200

#define CAKE_DSQ_NEW(tier)  (CAKE_DSQ_NEW_BASE + (tier))
#define CAKE_DSQ_OLD(tier)  (CAKE_DSQ_OLD_BASE + (tier))

#endif /* __CAKE_INTF_H */
