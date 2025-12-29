// SPDX-License-Identifier: GPL-2.0
/*
 * scx_cake - A sched_ext scheduler applying CAKE bufferbloat concepts
 *
 * This scheduler adapts CAKE's DRR++ (Deficit Round Robin++) algorithm
 * for CPU scheduling, providing low-latency scheduling for gaming and
 * interactive workloads.
 *
 * Key concepts from CAKE adapted here:
 * - Sparse flow detection: Low-CPU tasks (like gaming) get latency priority
 * - Direct dispatch: Waking tasks on idle CPUs run immediately
 * - Two-tier DSQ: Gaming/sparse tasks dispatched before normal tasks
 */

#include <scx/common.bpf.h>
#include "intf.h"

extern struct task_struct *scx_bpf_cpu_curr(s32 cpu) __ksym;
extern void bpf_rcu_read_lock(void) __ksym;
extern void bpf_rcu_read_unlock(void) __ksym;

char _license[] SEC("license") = "GPL";

/* Scheduler configuration (set by userspace before loading) */
const volatile u64 quantum_ns = CAKE_DEFAULT_QUANTUM_NS;
const volatile u64 new_flow_bonus_ns = CAKE_DEFAULT_NEW_FLOW_BONUS_NS;
const volatile u64 sparse_threshold = CAKE_DEFAULT_SPARSE_THRESHOLD;
const volatile u64 starvation_ns = CAKE_DEFAULT_STARVATION_NS;
const volatile bool enable_stats = false;  /* Set to true when --verbose is used */

/*
 * Global statistics (Per-CPU to avoid bus locking)
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct cake_stats);
} stats_map SEC(".maps");

/*
 * Global CPU Scoreboard (Wait-Free Status Array)
 * Replaces global atomic bitmasks with a distributed BSS array.
 *
 * - Wait-Free: Each CPU writes ONLY to its own slot (cpu_status[my_cpu]).
 * - False-Sharing Free: Explicit 64-byte padding per slot.
 * - Readers: Linear scan (fast on modern CPUs, avoids bus locking).
 */
struct cake_cpu_status cpu_status[CAKE_MAX_CPUS] SEC(".bss") __attribute__((aligned(64)));

static __always_inline struct cake_stats *get_local_stats(void)
{
    u32 key = 0;
    return bpf_map_lookup_elem(&stats_map, &key);
}

/* User exit info for graceful scheduler exit */
UEI_DEFINE(uei);

/* Global vtime removed to prevent bus locking. Tasks inherit vtime from parent. */

/* Optimization: Precomputed threshold to avoid division in hot path */
static u64 cached_threshold_ns;



/*
 * Seven dispatch queues - one per tier, served in priority order:
 * - CRITICAL_LATENCY_DSQ: Ultra-low latency (score=100 AND <250µs avg) - highest priority
 * - REALTIME_DSQ:    Ultra-sparse tasks (score=100, >=250µs avg) - very high priority
 * - CRITICAL_DSQ:    Very sparse tasks (audio, compositor) - high priority
 * - GAMING_DSQ:      Sparse/bursty tasks (game threads, UI) - gaming priority  
 * - INTERACTIVE_DSQ: Normal tasks (default applications) - baseline priority
 * - BATCH_DSQ:       Lower priority work (nice > 0) - lower priority
 * - BACKGROUND_DSQ:  Bulk tasks (compilers, encoders) - lowest priority
 *
 * SHARDING OPTIMIZATION:
 * Gaming and Interactive tiers are sharded (SCX_DSQ_SHARD_COUNT = 4).
 * We reserve ID space for them.
 */
#define CRITICAL_LATENCY_DSQ 0
#define REALTIME_DSQ    1
#define CRITICAL_DSQ    2
#define GAMING_DSQ      3                             /* Base: 3, 4, 5, 6 */
#define INTERACTIVE_DSQ (GAMING_DSQ + SCX_DSQ_SHARD_COUNT)     /* Base: 7, 8, 9, 10 */
#define BATCH_DSQ       (INTERACTIVE_DSQ + SCX_DSQ_SHARD_COUNT) /* 11 */
#define BACKGROUND_DSQ  (BATCH_DSQ + 1)               /* 12 */

/* Mapping from Tier ID (0-6) to Base DSQ ID */
static const u64 tier_to_dsq_base[8] = {
    CRITICAL_LATENCY_DSQ,
    REALTIME_DSQ,
    CRITICAL_DSQ,
    GAMING_DSQ,
    INTERACTIVE_DSQ,
    BATCH_DSQ,
    BACKGROUND_DSQ,
    BACKGROUND_DSQ /* Pad */
};

/*
 * Tier quantum multipliers (fixed-point, 1024 = 1.0x)
 * Higher tiers get SMALLER slices (more preemption points, lower latency)
 * Lower tiers get LARGER slices (less context switching for bulk work)
 * PADDED TO 8 for Branchless Access (tier & 7).
 */
static const u32 tier_multiplier[8] = {
    717,   /* Critical Latency: 0.7x (70%) -> 717/1024 */
    819,   /* Realtime:    0.8x (80%) -> 819/1024 */
    922,   /* Critical:    0.9x (90%) -> 922/1024 */
    1024,  /* Gaming:      1.0x (100%) -> 1024/1024 */
    1126,  /* Interactive: 1.1x (110%) -> 1126/1024 */
    1229,  /* Batch:       1.2x (120%) -> 1229/1024 */
    1331,  /* Background:  1.3x (130%) -> 1331/1024 */
    1024,  /* PADDING: Entry 7 (Unused/Safe) */
};

/* Sparse score thresholds for tier classification (0-100) */
#define THRESHOLD_GAMING      70   /* Score >= 70 → Gaming (only threshold actively used) */
#define CAKE_TIER_IDLE 255 /* Special value for Scoreboard (Cpu is Idle) */

/*
 * CAKE-style wait budget per tier (nanoseconds)
 * Tasks exceeding their tier's budget multiple times get demoted.
 * This prevents queue bloat and protects latency-sensitive tasks.
 * 
 * Formula: Starvation = 2x Wait Budget (consistent ratio)
 */
#define WAIT_BUDGET_CRITICAL_LATENCY 100000    /* 100µs - ultra-tight for input */
#define WAIT_BUDGET_REALTIME    750000    /* 750µs */
#define WAIT_BUDGET_CRITICAL    2000000   /* 2ms */
#define WAIT_BUDGET_GAMING      4000000   /* 4ms */
#define WAIT_BUDGET_INTERACTIVE 8000000   /* 8ms */
#define WAIT_BUDGET_BATCH       20000000  /* 20ms */

/* Array for O(1) wait budget lookup. PADDED TO 8 for Branchless Access (tier & 7). */
static const u64 wait_budget[8] = {
    WAIT_BUDGET_CRITICAL_LATENCY, /* Tier 0: Critical Latency - 100µs */
    WAIT_BUDGET_REALTIME,    /* Tier 1: Realtime - 750µs */
    WAIT_BUDGET_CRITICAL,    /* Tier 2: Critical - 2ms */
    WAIT_BUDGET_GAMING,      /* Tier 3: Gaming - 4ms */
    WAIT_BUDGET_INTERACTIVE, /* Tier 4: Interactive - 8ms */
    WAIT_BUDGET_BATCH,       /* Tier 5: Batch - 20ms */
    0,                       /* Tier 6: Background - no limit */
    0,                       /* PADDING: Entry 7 (Safe) */
};

/*
 * Per-tier starvation thresholds (nanoseconds)
 * Safety net: force preemption if task runs longer than its tier allows.
 * 
 * Formula: Starvation = 2x Wait Budget (except Background which has generous limit)
 */
#define STARVATION_CRITICAL_LATENCY 5000000  /* 5ms - 2x wait budget */
#define STARVATION_REALTIME     3000000    /* 3ms - 2x wait budget */
#define STARVATION_CRITICAL     4000000    /* 4ms - 2x wait budget */
#define STARVATION_GAMING       8000000    /* 8ms - 2x wait budget */
#define STARVATION_INTERACTIVE  16000000   /* 16ms - 2x wait budget */
#define STARVATION_BATCH        40000000   /* 40ms - 2x wait budget */
#define STARVATION_BACKGROUND   100000000  /* 100ms - generous for bulk work */

/* Array for O(1) starvation threshold lookup. PADDED TO 8 for Branchless Access (tier & 7). */
static const u64 starvation_threshold[8] = {
    STARVATION_CRITICAL_LATENCY, /* Tier 0: Critical Latency - 5ms */
    STARVATION_REALTIME,     /* Tier 1: Realtime - 1.5ms */
    STARVATION_CRITICAL,     /* Tier 2: Critical - 4ms */
    STARVATION_GAMING,       /* Tier 3: Gaming - 8ms */
    STARVATION_INTERACTIVE,  /* Tier 4: Interactive - 16ms */
    STARVATION_BATCH,        /* Tier 5: Batch - 40ms */
    STARVATION_BACKGROUND,   /* Tier 6: Background - 100ms */
    STARVATION_BACKGROUND,   /* PADDING: Entry 7 (Safe) */
};



/*
 * Vtime Table Removed:
 * FIFO DSQs do not use dsq_vtime for ordering.
 * Removed 160 bytes of static data + 30 cycles of math.
 */

/*
 * Per-task context map
 */
struct {
    __uint(type, BPF_MAP_TYPE_TASK_STORAGE);
    __uint(map_flags, BPF_F_NO_PREALLOC);
    __type(key, int);
    __type(value, struct cake_task_ctx);
} task_ctx SEC(".maps");

/*
 * Per-CPU preemption cooldown timestamp (ns)
 * Key: CPU ID
 * Value: Padded struct to prevent False Sharing (8 CPUs per cache line)
 */
struct cake_cooldown_elem {
    u64 last_preempt_ts;
    u8 __pad[56]; /* Pad to 64 bytes */
};

/*
 * Per-CPU preemption cooldown timestamp (ns)
 * REPLACED MAP WITH BSS ARRAY for Wait-Free Access.
 * ALIGNMENT: 64-byte aligned to prevent False Sharing.
 * SIZE: 256 to allow barrier-free verifier access (u8 range).
 */
struct cake_cooldown_elem cooldowns[256] SEC(".bss") __attribute__((aligned(64)));


/*
 * Bitfield Accessor Macros for packed_info
 * These allow us to pack multiple variables into a single u32.
 */

#define GET_WAIT_DATA(ctx) ((ctx->packed_info >> SHIFT_WAIT_DATA) & MASK_WAIT_DATA)
#define SET_WAIT_DATA(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_WAIT_DATA << SHIFT_WAIT_DATA)) | ((val & MASK_WAIT_DATA) << SHIFT_WAIT_DATA))

#define GET_SPARSE_SCORE(ctx) ((ctx->packed_info >> SHIFT_SPARSE_SCORE) & MASK_SPARSE_SCORE)
#define SET_SPARSE_SCORE(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_SPARSE_SCORE << SHIFT_SPARSE_SCORE)) | ((val & MASK_SPARSE_SCORE) << SHIFT_SPARSE_SCORE))

#define GET_TIER(ctx) ((ctx->packed_info >> SHIFT_TIER) & MASK_TIER)
#define SET_TIER(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_TIER << SHIFT_TIER)) | ((val & MASK_TIER) << SHIFT_TIER))

/*
 * Get or initialize task context
 */
static __always_inline struct cake_task_ctx *get_task_ctx(struct task_struct *p)
{
    struct cake_task_ctx *ctx;

    ctx = bpf_task_storage_get(&task_ctx, p, 0, 0);
    if (ctx)
        return ctx;

    /* Initialize new context */
    ctx = bpf_task_storage_get(&task_ctx, p, 0,
                               BPF_LOCAL_STORAGE_GET_F_CREATE);
    if (!ctx)
        return NULL;

    /* Initialize task context fields */
    ctx->next_slice = quantum_ns; /* Default slice */
    ctx->deficit_us = (u16)((quantum_ns + new_flow_bonus_ns) >> 10);
    ctx->last_run_at = 0;
    ctx->last_wake_ts = 0;
    ctx->avg_runtime_us = 0;
    
    /* Pack initial values: Err=255, Score=50, Tier=Interactive, Flags=New */
    u32 packed = 0;
    packed |= (255 & MASK_KALMAN_ERROR) << SHIFT_KALMAN_ERROR;
    packed |= (50  & MASK_SPARSE_SCORE) << SHIFT_SPARSE_SCORE;
    packed |= (CAKE_TIER_INTERACTIVE & MASK_TIER) << SHIFT_TIER;
    packed |= (CAKE_FLOW_NEW & MASK_FLAGS) << SHIFT_FLAGS;
    
    ctx->packed_info = packed;

    return ctx;
}

/*
 * Exponential Moving Average (EMA) for Runtime Estimation
 * 
 * Uses a simple IIR filter with alpha=1/8 for fast adaptation.
 * Formula: Avg += (New - Avg) >> 3
 * Cost: 3 ALU ops, 0 memory accesses.
 */


static __always_inline void update_kalman_estimate(struct cake_task_ctx *tctx, u64 runtime_ns)
{
    /* 
     * OPTIMIZATION: Revert to Bitwise EMA (Fix Regression)
     * The Kalman filter LUT + Multiply was too heavy (cache misses + ALU).
     * We use a "Bitwise EMA" with Alpha=1/8.
     * Formula: Avg = (Avg * 7 + New) / 8
     * Bitwise: Avg = ((Avg << 3) - Avg + New) >> 3
     * Cost: 0 Memory Accesses, 3 ALU ops.
     */
    u32 meas_us = (u32)(runtime_ns >> 10);
    u32 avg_us = tctx->avg_runtime_us;
    
    /* cap measurement to u16 max */
    if (meas_us > 65535) meas_us = 65535;

    if (avg_us == 0) {
        tctx->avg_runtime_us = (u16)meas_us;
    } else {
        /*
         * OPTIMIZATION: Standard IIR Filter
         * Formula: Avg += (New - Avg) >> 3
         * Instructions: SUB, SAR, ADD (3 ops). Saves 1 op over weighted sum.
         */
        s32 diff = (s32)meas_us - (s32)avg_us;
        tctx->avg_runtime_us = (u16)((s32)avg_us + (diff >> 3));
    }
}

/*
 * Update task tier based on sparse score and runtime (behavior-based)
 * 
 * Score ranges (7 tiers):
 *   100 + <50µs avg:   CritLatency - True input handlers
 *   100 + 50-500µs:    Realtime    - Fast sparse tasks (network IRQs)
 *   90-100 (fallback): Critical    - High-priority sparse
 *   70-89:  Gaming      - Sparse (game threads, UI)
 *   50-69:  Interactive - Baseline (default applications)
 *   30-49:  Batch       - Lower priority (nice > 0)
 *   0-29:   Background  - Bulk (compilers, encoders)
 *
 * OPTIMIZATION: Zero-Cycle Wakeup
 * We update the tier here (post-run) and store it in tctx.
 * This allows 'cake_enqueue' to be a simple O(1) load.
 */
static __always_inline void update_task_tier(struct task_struct *p, struct cake_task_ctx *tctx)
{
    u32 score = GET_SPARSE_SCORE(tctx);
    u32 avg_us = tctx->avg_runtime_us;
    
    /* OPTIMIZATION: Branchless Tier Selection (V2) */
    
    /* 1. Calculate Tier from Score (Math instead of Table) */
    /* Buckets: 0-29(6), 30-49(5), 50-69(4), 70-89(3), 90-100(2) */
    u8 tier;
    if (score < 30) {
        tier = 6;
    } else {
        /* 
         * Formula: 6 - ((Score - 10) / 20) works for 30-100
         * BITWISE: Replace / 20 with * 3277 >> 16 (0.050003 approx)
         * (x * 3277) >> 16 is exact for integers 0-100.
         */
        u32 val = (score - 10);
        u32 res = (val * 3277) >> 16;
        tier = 6 - (u8)res;
    }
    
    /* 2. Apply Latency Gates for score=100 (Bitwise logic) */
    /* If score==100 & avg>0... */
    
    bool is_100 = (score == 100);
    bool has_history = (avg_us > 0);
    bool check_gates = is_100 & has_history;
    
    /* adj = 2 if <50, 1 if <500, 0 otherwise */
    s32 adj = (avg_us < 500) + (avg_us < 50);
    s32 mask_gates = -(s32)check_gates; /* 0xFFFFFFFF if true */
    
    /* Apply adjustment if gates check passes */
    /* tier 2 (Critical) -> tier 1 (Realtime) or tier 0 (CritLatency) */
    tier -= (adj & mask_gates);
    
    SET_TIER(tctx, tier);

    /* 
     * OPTIMIZATION: Zero-Cycle Slice Calculation
     * Pre-compute the slice duration for the next wakeup.
     * This moves all math (deficits, bonuses, multipliers) out of the hot path.
     */
    u64 deficit_ns = (u64)tctx->deficit_us << 10;
    bool has_bonus = deficit_ns > quantum_ns;
    s64 mask_bonus = -(s64)has_bonus;
    u64 slice = (deficit_ns & mask_bonus) | (quantum_ns & ~mask_bonus);
    
    /* Apply tier multiplier and store */
    tctx->next_slice = (slice * tier_multiplier[tier & 7]) >> 10;
}

/*
 * Update sparse score based on runtime behavior
 * 
 * Score affects tier classification:
 *   90-100: Critical, 75-89: Gaming, 25-74: Interactive, 0-24: Background
 * 
 * We track promotions/demotions when crossing the Gaming threshold (75)
 * since this is the key boundary for gaming-relevant tasks.
 */
static __always_inline void update_sparse_score(struct cake_task_ctx *tctx, u64 runtime_ns)
{
    u32 old_score = GET_SPARSE_SCORE(tctx);
    u64 threshold_ns = cached_threshold_ns;

    /* 
     * OPTIMIZATION: Simple Score Update
     * Compiler will optimize this to CMOV (Conditional Move) or LEA.
     * Sparse=1 -> +4, Sparse=0 -> -6.
     */
    bool sparse = runtime_ns < threshold_ns;
    int change = sparse ? 4 : -6;
    int raw_score = (int)old_score + change;

    /* Clamp 0-100 (Compiler uses CMOV or simple branches) */
    if (raw_score < 0) raw_score = 0;
    else if (raw_score > 100) raw_score = 100;

    SET_SPARSE_SCORE(tctx, (u32)raw_score);

    if (enable_stats) {
        bool was_gaming = old_score >= THRESHOLD_GAMING;
        bool is_gaming = raw_score >= THRESHOLD_GAMING;
        
        if (was_gaming != is_gaming) {
             struct cake_stats *s = get_local_stats();
             if (s) {
                 if (is_gaming) s->nr_sparse_promotions++;
                 else s->nr_sparse_demotions++;
             }
        }
    }
}

/*
 * Select CPU for a waking task
 */
s32 BPF_STRUCT_OPS(cake_select_cpu, struct task_struct *p, s32 prev_cpu,
                   u64 wake_flags)
{
    struct cake_task_ctx *tctx;
    bool is_idle = false;
    s32 cpu;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx)) {
        /* Fallback if we can't get context */
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    u8 tier = GET_TIER(tctx);
    
    /*
     * FIX: Capture timestamp BEFORE fail-fast to ensure last_wake_ts is always set.
     * The wait budget system in cake_running relies on last_wake_ts > 0.
     * OPTIMIZATION: scx_bpf_now() uses cached rq->clock instead of TSC read.
     */
    u64 now = scx_bpf_now();
    tctx->last_wake_ts = (u32)now;
    
    /* 
     * OPTIMIZATION: Wait-Free Linear Scan (The Scoreboard)
     * Replaces Atomic Mask + CTZ.
     * - Checks Prev CPU first (Sticky).
     * - Scans valid CPUs from 0 to nr_cpus.
     * - No atomics, no locks, pure memory loads.
     */
    s32 best_idle = -1;
    s32 nr_cpus = scx_bpf_nr_cpu_ids();

    /* 1. Check Previous CPU (Sticky) */
    if (prev_cpu >= 0 && prev_cpu < CAKE_MAX_CPUS) {
        if (cpu_status[prev_cpu].is_idle) {
            best_idle = prev_cpu;
        }
    }

    /* 2. Linear Scan if Prev not idle */
    if (best_idle < 0) {
        /* 
         * OPTIMIZATION: Topology-Aware Circular Scan
         * Scan forward from prev_cpu to maximize cache locality (neighbors).
         * Includes "Hardware Prefetching" via __builtin_prefetch.
         */
        s32 start = prev_cpu + 1;
        if (start < 0) start = 0;

        /* Loop 1: Start -> End */
        if (start < CAKE_MAX_CPUS && start < nr_cpus) {
            #pragma unroll 4
            for (s32 i = start; i < CAKE_MAX_CPUS; i++) {
                if (i >= nr_cpus) break;

                /* Hardware Call: Prefetch next line to hide latency */
                if (i + 1 < nr_cpus)
                    __builtin_prefetch(&cpu_status[i + 1]);

                if (cpu_status[i].is_idle) {
                    best_idle = i;
                    goto found_idle;
                }
            }
        }

        /* Loop 2: 0 -> Start */
        #pragma unroll 4
        for (s32 i = 0; i < CAKE_MAX_CPUS; i++) {
            if (i >= start) break; /* Wrapped around to where we started */
            if (i >= nr_cpus) break;

            /* Hardware Call: Prefetch next line */
            if (i + 1 < start)
                __builtin_prefetch(&cpu_status[i + 1]);

            if (cpu_status[i].is_idle) {
                best_idle = i;
                goto found_idle;
            }
        }
    }

found_idle:
    if (best_idle >= 0) {
        cpu = best_idle;
        is_idle = true;
    } else {
        /* No idle CPU found - use default selection which handles SMT/LLC checks if needed */
        cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    /* Direct dispatch if idle CPU found - bypasses DSQ entirely */
    if (is_idle) {
        /* Use SCX_ENQ_LAST to skip redundant enqueue call - saves 5-20µs */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, quantum_ns, SCX_ENQ_LAST);
        if (enable_stats) {
            struct cake_stats *s = get_local_stats();
            if (s) s->nr_new_flow_dispatches++;
        }
        return cpu;  /* Critical: return early to avoid cake_enqueue */
    }

    /* 
     * Preemption Injection (Latency path)
     * If saturated, check if we can preempt a lower priority task.
     */
    if (tier <= REALTIME_DSQ) { // Tier 0 (CritLatency) or Tier 1 (Realtime)
        s32 victim_cpu = -1;

        /* 
         * OPTIMIZATION: Topology-Aware Circular Scan for Victim
         * Prioritize preempting neighbors to keep work local.
         */
        s32 start = prev_cpu + 1;
        if (start < 0) start = 0;

        /* Loop 1: Start -> End */
        if (start < CAKE_MAX_CPUS && start < nr_cpus) {
            #pragma unroll 4
            for (s32 i = start; i < CAKE_MAX_CPUS; i++) {
                if (i >= nr_cpus) break;

                if (i + 1 < nr_cpus) __builtin_prefetch(&cpu_status[i + 1]);

                if (cpu_status[i].tier >= INTERACTIVE_DSQ) {
                    victim_cpu = i;
                    goto found_victim;
                }
            }
        }

        /* Loop 2: 0 -> Start */
        #pragma unroll 4
        for (s32 i = 0; i < CAKE_MAX_CPUS; i++) {
            if (i >= start) break;
            if (i >= nr_cpus) break;

            if (i + 1 < start) __builtin_prefetch(&cpu_status[i + 1]);

            if (cpu_status[i].tier >= INTERACTIVE_DSQ) {
                victim_cpu = i;
                goto found_victim;
            }
        }
        
found_victim:
        /* Did we find a victim? */
        if (victim_cpu >= 0) {
            /* 
             * OPTIMIZATION: Direct BSS Access (Wait-Free & Barrier-Free)
             */
            struct cake_cooldown_elem *cooldown = &cooldowns[victim_cpu];
            
            /* Relaxed load is fine for heuristic */
            u64 last_ts = *(volatile u64 *)&cooldown->last_preempt_ts;
            
            if (now - last_ts > 50000) {
                 /* Relaxed store */
                *(volatile u64 *)&cooldown->last_preempt_ts = now;
                
                scx_bpf_kick_cpu(victim_cpu, SCX_KICK_PREEMPT);
                
                if (enable_stats) {
                    struct cake_stats *s = get_local_stats();
                    if (s) s->nr_input_preempts++;
                }

                /* Try to queue on victim CPU */
                cpu = victim_cpu;
            }
        }
    }

    return cpu;
}

/*
 * Enqueue task to the appropriate DSQ based on sparse detection
 */
void BPF_STRUCT_OPS(cake_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct cake_task_ctx *tctx;
    u8 tier;
    u64 dsq_id;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx)) {
        /* Fallback: dispatch to interactive DSQ */
        scx_bpf_dsq_insert(p, INTERACTIVE_DSQ, quantum_ns, enq_flags);
        return;
    }

    /* Zero-Cycle Wakeup: Tier already calculated in cake_stopping */
    tier = GET_TIER(tctx);

    /* Track if this is a wakeup (new flow) or preemption (old flow) */
    if (enable_stats) {
        struct cake_stats *s = get_local_stats();
        if (s) {
            if (enq_flags & SCX_ENQ_WAKEUP) {
                s->nr_new_flow_dispatches++;
            } else {
                s->nr_old_flow_dispatches++;
            }

            /* 
             * Bound tier for stats array access using bitmask
             * BPF verifier requires provably bounded indices
             * tier & 0x7 ensures max value is 7 (CAKE_TIER_MAX-1 = 6)
             */
            u8 bounded_tier = tier & 0x7;
            if (bounded_tier < CAKE_TIER_MAX)
                s->nr_tier_dispatches[bounded_tier]++;
        }
    }

    /*
     * Route to DSQ based on tier classification:
     * - Critical Latency: CRITICAL_LATENCY_DSQ (true input handlers)
     * - Realtime: REALTIME_DSQ (score=100, longer runtime)
     * - Critical: CRITICAL_DSQ (audio, compositor)
     * - Gaming: GAMING_DSQ (sparse/bursty tasks)
     * - Interactive: INTERACTIVE_DSQ (normal applications)
     * - Batch: BATCH_DSQ (nice > 0)
     * - Background: BACKGROUND_DSQ (bulk work)
     */

    /*
     * OPTIMIZATION: Look up base DSQ from table
     * This handles the gaps caused by sharding.
     */
    dsq_id = tier_to_dsq_base[tier & 7];

    /* 
     * OPTIMIZATION: Stochastic Sharding for High-Traffic Tiers
     * Distribute tasks across shards to reduce DSQ lock contention.
     * Use (pid ^ cpu) for stateless distribution.
     */
    if (tier == CAKE_TIER_GAMING || tier == CAKE_TIER_INTERACTIVE) {
        u32 hash = (p->pid ^ bpf_get_smp_processor_id());
        dsq_id += (hash & SCX_DSQ_SHARD_MASK);
    }

    /*
     * OPTIMIZATION: Zero-Cycle Slice (Pre-Computed)
     * All the complex deficit/bonus/multiplier logic was done in cake_stopping.
     * Now we just load the result. 15 cycles -> 0 cycles.
     */
    u64 slice = tctx->next_slice;

    scx_bpf_dsq_insert(p, dsq_id, slice, enq_flags);
}

/*
 * Dispatch tasks to run on this CPU
 * 
 * DSQs are served in strict priority order:
 *   1. Critical Latency (true input handlers - score=100, <250µs avg)
 *   2. Realtime (score=100, >=250µs avg)
 *   3. Critical (audio, compositor)
 *   4. Gaming (sparse/bursty)
 *   5. Interactive (normal apps)
 *   6. Batch (nice > 0)
 *   7. Background (bulk work)
 *
 * Starvation protection: Probabilistically check lower tiers
 * even if higher tiers have work, to prevent complete starvation.
 *
 * OPTIMIZATION: Removed global dispatch_count which caused cache-line
 * contention across all CPUs. Now uses timestamp-based probabilistic
 * approach - roughly every 16 dispatches per-CPU without contention.
 */
void BPF_STRUCT_OPS(cake_dispatch, s32 cpu, struct task_struct *prev)
{
    /*
     * Starvation protection: probabilistically let lower tiers run
     * OPTIMIZATION: "Jitter Entropy" (0 Cycles)
     * Use sum_exec_runtime (nanosecond precision) which captures natural
     * system jitter (cache misses, interrupts).
     * XOR with PID to scatter. Avoids helper overhead of prandom.
     */
    if (prev && ((prev->pid ^ prev->se.sum_exec_runtime) & 0xF) == 0) {
        /* Give background a chance even if others have work */
        if (scx_bpf_dsq_nr_queued(BACKGROUND_DSQ) && scx_bpf_dsq_move_to_local(BACKGROUND_DSQ))
            return;
        if (scx_bpf_dsq_nr_queued(INTERACTIVE_DSQ) && scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ))
            return;
    }
    
    /* Priority order: Critical Latency → Realtime → Critical → Gaming → Interactive → Batch → Background */

    /* Non-sharded high priority tiers */
    if (scx_bpf_dsq_nr_queued(CRITICAL_LATENCY_DSQ) && scx_bpf_dsq_move_to_local(CRITICAL_LATENCY_DSQ))
        return;
    if (scx_bpf_dsq_nr_queued(REALTIME_DSQ) && scx_bpf_dsq_move_to_local(REALTIME_DSQ))
        return;
    if (scx_bpf_dsq_nr_queued(CRITICAL_DSQ) && scx_bpf_dsq_move_to_local(CRITICAL_DSQ))
        return;

    /*
     * Sharded Gaming Tier
     * Check local shard first (cpu % mask), then scan others.
     * Loop unrolled by compiler (4 iterations).
     */
    s32 i;
    u64 base = GAMING_DSQ;
    /* Try local shard first for affinity */
    u32 local_shard = cpu & SCX_DSQ_SHARD_MASK;
    if (scx_bpf_dsq_nr_queued(base + local_shard) && scx_bpf_dsq_move_to_local(base + local_shard))
        return;

    /* Scan other shards */
    for (i = 0; i < SCX_DSQ_SHARD_COUNT; i++) {
        if (i == local_shard) continue;
        if (scx_bpf_dsq_nr_queued(base + i) && scx_bpf_dsq_move_to_local(base + i))
            return;
    }

    /*
     * Sharded Interactive Tier
     */
    base = INTERACTIVE_DSQ;
    if (scx_bpf_dsq_nr_queued(base + local_shard) && scx_bpf_dsq_move_to_local(base + local_shard))
        return;
    for (i = 0; i < SCX_DSQ_SHARD_COUNT; i++) {
        if (i == local_shard) continue;
        if (scx_bpf_dsq_nr_queued(base + i) && scx_bpf_dsq_move_to_local(base + i))
            return;
    }

    /* Non-sharded low priority tiers */
    if (scx_bpf_dsq_nr_queued(BATCH_DSQ) && scx_bpf_dsq_move_to_local(BATCH_DSQ))
        return;
    if (scx_bpf_dsq_nr_queued(BACKGROUND_DSQ))
        scx_bpf_dsq_move_to_local(BACKGROUND_DSQ);
}

/*
 * Task is starting to run
 */
void BPF_STRUCT_OPS(cake_running, struct task_struct *p)
{
    struct cake_task_ctx *tctx;
    /* OPTIMIZATION: scx_bpf_now() uses cached rq->clock (~3-5 cycles vs ~15-25 for TSC) */
    u64 now = scx_bpf_now();

    tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

    /* 
     * OPTIMIZATION: Wait-Free Scoreboard Update
     * Update the local CPU's tier in the global BSS array.
     * Replaces the Map Lookup + Atomic Mask logic.
     */
    u8 tier = GET_TIER(tctx);
    s32 cpu = scx_bpf_task_cpu(p);

    if (cpu >= 0 && cpu < CAKE_MAX_CPUS) {
        /* Check if changed to avoid dirtying cache line needlessly */
        if (cpu_status[cpu].tier != tier) {
            cpu_status[cpu].tier = tier;
        }
    }

    /* WAIT BUDGET CHECK (CAKE AQM) */
    if (tctx->last_wake_ts > 0) {
        u32 now_ts = (u32)now;
        /* Handle wrap-around (native u32 subtraction) */
        u64 wait_time = (u64)(now_ts - tctx->last_wake_ts); /* Roughly * 1024 */
        
        /* 
         * OPTIMIZATION: Bitwise Decay on Long Sleep
         * If task slept for > 33ms (33,000,000ns), decay its history.
         * Tuned for Gaming: 33ms is ~2 frames. Resets "heavy" stats faster
         * for bursty threads (e.g. loading screens -> gameplay).
         * >> 1 is 50% decay (cheap 0-cycle logic vs Kalman reset).
         */
        if (wait_time > 33000000) {
             tctx->avg_runtime_us >>= 1;
        }

        /* Tier read moved up for scoreboard */
        
        /* OPTIMIZATION: Single stats lookup, reused throughout function */
        struct cake_stats *stats = enable_stats ? get_local_stats() : NULL;
        
        /* Global and per-tier stats (only when stats enabled) */
        if (stats) {
            stats->total_wait_ns += wait_time;
            stats->nr_waits++;
            
            /* Update max (race possible but per-cpu minimizes it) */
            if (wait_time > stats->max_wait_ns)
                stats->max_wait_ns = wait_time;
            
            /* Per-tier stats */
            if (tier < CAKE_TIER_MAX) {
                stats->total_wait_ns_tier[tier] += wait_time;
                stats->nr_waits_tier[tier]++;
                if (wait_time > stats->max_wait_ns_tier[tier])
                    stats->max_wait_ns_tier[tier] = wait_time;
            }
        }
        

        /* Wait budget tracking (Packet Logic: 4-bit counters) */
        u8 wait_data = GET_WAIT_DATA(tctx);
        u8 checks = wait_data & 0xF;
        u8 violations = wait_data >> 4;
        
        checks++;
        
        /* 
         * Check budget:
         * Critical: 500us, Gaming: 2ms, Interactive: 10ms
         * OPTIMIZATION: Branchless access (tier & 7) - Padded array
         */
        u64 budget_ns = wait_budget[tier & 7];
        if (wait_time > budget_ns) {
            violations++;
        }
        

        
        /* Re-pack and save */
        if (checks >= 15) checks = 15;
        if (violations >= 15) violations = 15;
        SET_WAIT_DATA(tctx, (violations << 4) | checks);

        /* Check for demotion (Ratio > 25%) */
        /* Since we don't have exact 'wait_checks' separate from sample loop, 
           we rely on the 10-sample window reset */
        
        if (checks >= 10 && tier < CAKE_TIER_BACKGROUND) {
            if (violations >= 3) { /* > 30% bad waits */
                /* Penalize score to force drop */
                u32 current_score = GET_SPARSE_SCORE(tctx);
                u32 penalized = (current_score > 10) ? current_score - 10 : 0;
                SET_SPARSE_SCORE(tctx, penalized);
                
                /* Reset data */
                SET_WAIT_DATA(tctx, 0);
                
                /* Reuse hoisted stats pointer - saves redundant lookup */
                if (stats) {
                    stats->nr_wait_demotions++;
                    if (tier < CAKE_TIER_MAX)
                        stats->nr_wait_demotions_tier[tier]++;
                }
            } else {
                /* Good behavior - reset */
                /* Reset data */
                SET_WAIT_DATA(tctx, 0);
            }
        }
        
        /* Clear last_wake_at to prevent double-counting if task runs again */
        tctx->last_wake_ts = 0;
    }

    tctx->last_run_at = (u32)now;
}

/*
 * Task is stopping (yielding or being preempted)
 */
void BPF_STRUCT_OPS(cake_stopping, struct task_struct *p, bool runnable)
{
    struct cake_task_ctx *tctx;
    /* OPTIMIZATION: scx_bpf_now() uses cached rq->clock */
    u64 now = scx_bpf_now();
    u64 runtime;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx || tctx->last_run_at == 0))
        return;

    /* u32 automatic wrap handling for runtime */
    u32 now_32 = (u32)now;
    u32 runtime_32 = now_32 - tctx->last_run_at;
    runtime = (u64)runtime_32;
    
    /* Update Kalman Filter Estimate (HFT Math) */
    update_kalman_estimate(tctx, runtime);

    /* Update sparse score - this determines Gaming vs Normal DSQ */
    update_sparse_score(tctx, runtime);

    /* 
     * OPTIMIZATION: Removed Dead Vtime Code
     * Since we use FIFO queues, updating p->scx.dsq_vtime has NO EFFECT.
     * Saved ~30 cycles of pointless math.
     */

    /* Update deficit (us) */
    /* deficit is u16 (us), runtime is u64 (ns). Convert runtime to us (>> 10) */
    u32 runtime_us = (u32)(runtime >> 10);
    if (runtime_us < tctx->deficit_us)
        tctx->deficit_us -= (u16)runtime_us;
    else
        tctx->deficit_us = 0;

    /* 
     * OPTIMIZATION: Calc Tier for Next Wakeup (Zero-Cycle Enqueue)
     * We calculate the tier NOW (while data is hot in L1) and save it.
     * When this task wakes up later, it can just read the result.
     * MOVED: Must run AFTER deficit update to ensure next_slice uses current credit!
     */
    update_task_tier(p, tctx);
}

/*
 * Periodic tick - check for starvation with per-tier thresholds
 * 
 * Each tier has its own starvation limit:
 *   Critical:    5ms  - Input handlers shouldn't run this long
 *   Gaming:      10ms - Game threads need responsiveness
 *   Interactive: 20ms - Normal apps get more leeway
 *   Background:  50ms - Bulk work can run longer
 */
void BPF_STRUCT_OPS(cake_update_idle, s32 cpu, bool idle)
{
    /*
     * OPTIMIZATION: Wait-Free Scoreboard Update
     * Replaces atomic bitmask ops with a simple store to the local CPU's BSS slot.
     * No atomics, no bus locking. 100% false-sharing free due to padding.
     */
    if (cpu >= 0 && cpu < CAKE_MAX_CPUS) {
        /*
         * Note: We use a volatile cast or direct assignment.
         * Since only THIS cpu writes to THIS slot, ordering is implicit.
         * The reader loops will see the update eventually (relaxed).
         */
         cpu_status[cpu].is_idle = idle ? 1 : 0;
    }
}

void BPF_STRUCT_OPS(cake_tick, struct task_struct *p)
{
    struct cake_task_ctx *tctx;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

    /* Check for starvation using tier-specific threshold */
    if (tctx->last_run_at > 0) {    /* Check for starvation */
        /* Use u32 wrap-safe math: (u32)now - last_run_at */
        /* Only valid if we ran within 4 seconds, otherwise we assume starvation anyway */
        /* OPTIMIZATION: scx_bpf_now() uses cached rq->clock */
        u32 runtime_32 = (u32)scx_bpf_now() - tctx->last_run_at;
        u64 runtime = (u64)runtime_32;

        u8 tier = GET_TIER(tctx);
        
        /* O(1) threshold lookup via padded array (tier & 7) */
        u64 threshold = starvation_threshold[tier & 7];
        
        if (runtime > threshold) {
            /* Force preemption - task exceeded its tier's starvation limit */
            scx_bpf_kick_cpu(scx_bpf_task_cpu(p), SCX_KICK_PREEMPT);
            
            /* Track per-tier starvation preempts */
            if (enable_stats && tier < CAKE_TIER_MAX) {
                struct cake_stats *s = get_local_stats();
                if (s) s->nr_starvation_preempts_tier[tier]++;
            }
        }
    }
}

/*
 * Task is enabled (joining sched_ext)
 */
void BPF_STRUCT_OPS(cake_enable, struct task_struct *p)
{
    /* No initialization needed - context created on first use */
}

/*
 * Task is disabled (leaving sched_ext)
 */
void BPF_STRUCT_OPS(cake_disable, struct task_struct *p)
{
    /* 
     * Cleanup Task Storage
     * When a task leaves scx_cake (e.g. exit or switch to CFS), we must
     * explicitly delete its task storage to prevent memory leaks.
     * Use BPF helper which is faster than waiting for task rcu death.
     */
    bpf_task_storage_delete(&task_ctx, p);
}

/*
 * Initialize the scheduler
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(cake_init)
{
    s32 ret;

    /* BITWISE OPTIMIZATION: >> 10 (~ / 1024) instead of / 1000 */
    cached_threshold_ns = (quantum_ns * sparse_threshold) >> 10;

    /* Initialize Scoreboard (Correctness Sync) */
    u32 nr_cpus = scx_bpf_nr_cpu_ids();

    /* Loop bounded for verifier */
    for (s32 i = 0; i < CAKE_MAX_CPUS; i++) {
        if (i >= nr_cpus) break;
        bpf_rcu_read_lock();
        struct task_struct *p = scx_bpf_cpu_curr(i);
        /* If idle task is running, set the status */
        if (p && p->pid == 0) {
            cpu_status[i].is_idle = 1;
        } else {
            cpu_status[i].is_idle = 0;
        }
        /* Initialize tier to background/safe default */
        cpu_status[i].tier = CAKE_TIER_BACKGROUND;
        bpf_rcu_read_unlock();
    }

    /* Create all 7 dispatch queues in priority order */
    ret = scx_bpf_create_dsq(CRITICAL_LATENCY_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(REALTIME_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(CRITICAL_DSQ, -1);
    if (ret < 0)
        return ret;

    /* Create Sharded Gaming DSQs */
    for (s32 i = 0; i < SCX_DSQ_SHARD_COUNT; i++) {
        ret = scx_bpf_create_dsq(GAMING_DSQ + i, -1);
        if (ret < 0) return ret;
    }

    /* Create Sharded Interactive DSQs */
    for (s32 i = 0; i < SCX_DSQ_SHARD_COUNT; i++) {
        ret = scx_bpf_create_dsq(INTERACTIVE_DSQ + i, -1);
        if (ret < 0) return ret;
    }

    ret = scx_bpf_create_dsq(BATCH_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(BACKGROUND_DSQ, -1);
    if (ret < 0)
        return ret;

    return 0;
}

/*
 * Scheduler exit - record exit info
 */
void BPF_STRUCT_OPS(cake_exit, struct scx_exit_info *ei)
{
    UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(cake_ops,
               .select_cpu     = (void *)cake_select_cpu,
               .enqueue        = (void *)cake_enqueue,
               .dispatch       = (void *)cake_dispatch,
               .running        = (void *)cake_running,
               .stopping       = (void *)cake_stopping,
               .update_idle    = (void *)cake_update_idle,
                .tick           = (void *)cake_tick,
               .enable         = (void *)cake_enable,
               .disable        = (void *)cake_disable,
               .init           = (void *)cake_init,
               .exit           = (void *)cake_exit,
               .flags          = SCX_OPS_KEEP_BUILTIN_IDLE,
               .name           = "cake");
