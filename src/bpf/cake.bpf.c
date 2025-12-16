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
const volatile bool debug = false;
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
 * Global Idle Count (Fail-Fast Optimization)
 * Atomic counter of currently idle CPUs.
 * Used to skip expensive search in select_cpu when 0.
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} idle_stats_map SEC(".maps");

static __always_inline struct cake_stats *get_local_stats(void)
{
    u32 key = 0;
    return bpf_map_lookup_elem(&stats_map, &key);
}

/* User exit info for graceful scheduler exit */
UEI_DEFINE(uei);

/* Global vtime for fair scheduling */
static u64 vtime_now;

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
 */
#define CRITICAL_LATENCY_DSQ 0
#define REALTIME_DSQ    1
#define CRITICAL_DSQ    2
#define GAMING_DSQ      3
#define INTERACTIVE_DSQ 4
#define BATCH_DSQ       5
#define BACKGROUND_DSQ  6

/*
 * Tier quantum multipliers (stored as percentage)
 * Higher tiers get SMALLER slices (more preemption points, lower latency)
 * Lower tiers get LARGER slices (less context switching for bulk work)
 */
/*
 * Tier quantum multipliers (stored as percentage)
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

/* Sparse score thresholds for tier classification (0-100)
 * Each tier is determined by the sparse score range:
 * - 100:    Realtime (perfect sparse score only)
 * - 90-99:  Critical (ultra-sparse, input handlers)
 * - 70-89:  Gaming (sparse, interactive)
 * - 50-69:  Interactive (normal applications)
 * - 30-49:  Batch (lower priority)
 * - 0-29:   Background (bulk/heavy CPU users)
 */
#define THRESHOLD_REALTIME    100  /* Score == 100 → Realtime */
#define THRESHOLD_CRITICAL    90   /* Score >= 90 → Critical */
#define THRESHOLD_GAMING      70   /* Score >= 70 → Gaming */
#define THRESHOLD_INTERACTIVE 50   /* Score >= 50 → Interactive */
#define THRESHOLD_BATCH       30   /* Score >= 30 → Batch */
/* Below 30 → Background */

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
#define WAIT_VIOLATIONS_DEMOTE  4         /* Demote after N consecutive violations */

/* Array for O(1) wait budget lookup (replaces switch statement in hot path) */
/* Array for O(1) wait budget lookup (replaces switch statement in hot path) */
/* PADDED TO 8 for Branchless Access (tier & 7). */
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

/* Array for O(1) starvation threshold lookup (avoids switch in hot path) */
/* Array for O(1) starvation threshold lookup (avoids switch in hot path) */
/* PADDED TO 8 for Branchless Access (tier & 7). */
static const u64 starvation_threshold[8] = {
    STARVATION_CRITICAL_LATENCY, /* Tier 0: Critical Latency - 200µs */
    STARVATION_REALTIME,     /* Tier 1: Realtime - 1.5ms */
    STARVATION_CRITICAL,     /* Tier 2: Critical - 4ms */
    STARVATION_GAMING,       /* Tier 3: Gaming - 8ms */
    STARVATION_INTERACTIVE,  /* Tier 4: Interactive - 16ms */
    STARVATION_BATCH,        /* Tier 5: Batch - 40ms */
    STARVATION_BACKGROUND,   /* Tier 6: Background - 100ms */
    STARVATION_BACKGROUND,   /* PADDING: Entry 7 (Safe) */
};



/*
 * Inverse weight table for vtime scaling (Nice -20 to +19)
 * Maps static_prio (100-139) to inverse factor.
 * Factor = (1024 * 65536) / sched_prio_to_weight[load]
 * Allows vtime = (slice * factor) >> 16
 */
static const u32 sched_prio_to_w_inv[40] = {
    756, 935, 1188, 1450, 1849, 2301, 2885, 3587, 4489, 5631,
    7028, 8806, 11001, 13684, 17180, 21502, 26832, 33706, 42313, 52551,
    65536, /* Nice 0 */
    81840, 102456, 127583, 158649, 200324, 246723, 312134, 390167, 489845,
    610080, 771366, 958698, 1198372, 1491308, 1864135, 2314098, 2917776, 3728270, 4473924
};

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
 * Value: Last time we forced a preemption on this CPU
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1024); /* Support up to 1024 CPUs */
    __type(key, u32);
    __type(value, u64);
} preempt_cooldown SEC(".maps");


/*
 * Bitfield Accessor Macros for 16-byte struct
 * These allow us to pack 5 variables into a single u32.
 */
#define GET_KALMAN_ERROR(ctx) ((ctx->packed_info >> SHIFT_KALMAN_ERROR) & MASK_KALMAN_ERROR)
#define SET_KALMAN_ERROR(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_KALMAN_ERROR << SHIFT_KALMAN_ERROR)) | ((val & MASK_KALMAN_ERROR) << SHIFT_KALMAN_ERROR))

#define GET_WAIT_DATA(ctx) ((ctx->packed_info >> SHIFT_WAIT_DATA) & MASK_WAIT_DATA)
#define SET_WAIT_DATA(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_WAIT_DATA << SHIFT_WAIT_DATA)) | ((val & MASK_WAIT_DATA) << SHIFT_WAIT_DATA))

#define GET_SPARSE_SCORE(ctx) ((ctx->packed_info >> SHIFT_SPARSE_SCORE) & MASK_SPARSE_SCORE)
#define SET_SPARSE_SCORE(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_SPARSE_SCORE << SHIFT_SPARSE_SCORE)) | ((val & MASK_SPARSE_SCORE) << SHIFT_SPARSE_SCORE))

#define GET_TIER(ctx) ((ctx->packed_info >> SHIFT_TIER) & MASK_TIER)
#define SET_TIER(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_TIER << SHIFT_TIER)) | ((val & MASK_TIER) << SHIFT_TIER))

#define GET_FLAGS(ctx) ((ctx->packed_info >> SHIFT_FLAGS) & MASK_FLAGS)
#define SET_FLAGS(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_FLAGS << SHIFT_FLAGS)) | ((val & MASK_FLAGS) << SHIFT_FLAGS))

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

    /* Initialize 16-byte compressed fields */
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
 * Integer-based Kalman Filter for Runtime Prediction
 * 
 * Tracks both the estimate (avg_runtime) and the uncertainty (error).
 * Allows instant adaptation to mode switches (Menu -> Game) while
 * filtering out random OS jitter.
 * 
 * Math:
 *   K = Error / (Error + Noise)
 *   Est = Est + K * (Measure - Est)
 *   Error = (1 - K) * Error
 */
/* 
 * Pre-calculated Kalman Gain (K) Lookup Table
 * Formula: K_256 = ((err+10)*256) / ((err+10)+50)
 * Avoids expensive runtime division.
 */
/* 
 * Pre-calculated Kalman Gain (K) & Next Error Lookup Table (Double-Packed)
 * Formula: 
 *   High Byte: Next Error = ((256-K)*(err+10)) >> 8
 *   Low Byte:  Gain (K)   = ((err+10)*256) / ((err+10)+50)
 * Avoids ALL runtime arithmetic for error update.
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
 * Classify task tier based on sparse score and runtime (behavior-based)
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
 * OPTIMIZATION: Uses lookup table with latency gates for score=100.
 */
static __always_inline u8 classify_tier(struct task_struct *p, struct cake_task_ctx *tctx)
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
    
    return tier;
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
    u64 now = bpf_ktime_get_ns();

    tctx = get_task_ctx(p);
    if (unlikely(!tctx)) {
        /* Fallback if we can't get context */
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    /* 
     * OPTIMIZATION: Fail-Fast Idle Check (Saturation)
     * If NO CPUs are idle (count == 0), skip the 100-cycle search.
     * We know it will fail. Just return prev_cpu (likely busy) 
     * and let it queue to global DSQ.
     */
    u32 idle_key = 0;
    u32 *idle_count = bpf_map_lookup_elem(&idle_stats_map, &idle_key);
    if (idle_count && *idle_count == 0) {
        /* No idle CPUs exists. Skip expensive LLC search. */
        return prev_cpu;
    }

    /* Track wake time for wait budget calculations (Store full u32) */
    tctx->last_wake_ts = (u32)now;

    /* 
     * OPTIMIZATION: Sticky Fast Path (Cache Locality)
     * If the previous CPU is idle, pick it immediately.
     * Logic: Check if current task on prev_cpu is PID 0 (Idle/Swapper).
     * Prevents game threads from hopping cores and losing L1/L2 cache.
     */
    if (prev_cpu >= 0) {
        struct task_struct *curr = scx_bpf_cpu_curr(prev_cpu);
        if (curr && curr->pid == 0) {
            is_idle = true; 
            cpu = prev_cpu;
        } else {
            /* Try to find an idle CPU, prefer same LLC */
            cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
        }
    } else {
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
     * Preemption Injection
     * If we didn't find an idle CPU, but this is a latency-sensitive task,
     * check if the target CPU is running something low priority.
     */
    if (GET_SPARSE_SCORE(tctx) >= 100) {
        struct task_struct *curr = scx_bpf_cpu_curr(cpu);
        struct cake_task_ctx *curr_ctx = get_task_ctx(curr);
        
        /* If current task is Interactive, Batch or Background, it's a valid victim */
        if (curr_ctx && GET_TIER(curr_ctx) >= CAKE_TIER_INTERACTIVE) {
            u32 cpu_key = cpu;
            u64 *last_preempt;
            
            last_preempt = bpf_map_lookup_elem(&preempt_cooldown, &cpu_key);
            if (last_preempt) {
                if (now - *last_preempt > 50000) {
                    *last_preempt = now;
                    scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
                    
                    if (enable_stats) {
                        struct cake_stats *s = get_local_stats();
                        if (s) s->nr_input_preempts++;
                    }
                }
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

    /* Classify tier */
    tier = classify_tier(p, tctx);
    SET_TIER(tctx, tier);

    /* Track if this is a wakeup (new flow) or preemption (old flow) */
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
     * OPTIMIZATION: Direct mapping (TIER_ID == DSQ_ID)
     * Removes switch branch table. 
     * Safe because tier is guaranteed 0-6 by logic and 0-7 by type.
     */
    dsq_id = tier;

    /*
     * OPTIMIZATION: New flow bonus via slice adjustment
     * New flows have higher deficit (quantum + bonus), so give them
     * a longer initial slice. This effectively prioritizes them without
     * needing vtime/PRIQ ordering.
     */
    /*
     * OPTIMIZATION: Branchless New Flow Bonus
     * Use bitwise mask to select larger of quantum or deficit.
     * Conversion: deficit_us << 10 to get approx nanoseconds
     */
    u64 deficit_ns = (u64)tctx->deficit_us << 10;
    bool has_bonus = deficit_ns > quantum_ns;
    s64 mask_bonus = -(s64)has_bonus;
    u64 slice = (deficit_ns & mask_bonus) | (quantum_ns & ~mask_bonus);

    /*
     * Apply tier-based quantum multiplier
     * Higher tiers get SMALLER slices (more responsive)
     * Lower tiers get LARGER slices (less context switching)
     */
    /* OPTIMIZATION: Unconditional fixed-point math (>> 10) */
    /* Access safe: tier_multiplier is padded to 8, (tier & 7) is 0-7 */
    slice = (slice * tier_multiplier[tier & 7]) >> 10;

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
        if (scx_bpf_dsq_move_to_local(BACKGROUND_DSQ))
            return;
        if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ))
            return;
    }
    
    /* Priority order: Critical Latency → Realtime → Critical → Gaming → Interactive → Batch → Background */
    if (scx_bpf_dsq_move_to_local(CRITICAL_LATENCY_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(REALTIME_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(CRITICAL_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(GAMING_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(BATCH_DSQ))
        return;
    scx_bpf_dsq_move_to_local(BACKGROUND_DSQ);
}

/*
 * Task is starting to run
 */
void BPF_STRUCT_OPS(cake_running, struct task_struct *p)
{
    struct cake_task_ctx *tctx;
    u64 now = bpf_ktime_get_ns();

    tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

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

        u8 tier = GET_TIER(tctx);
        
        /* Global and per-tier stats (only when stats enabled) */
        if (enable_stats) {
            struct cake_stats *s = get_local_stats();
            if (s) {
                s->total_wait_ns += wait_time;
                s->nr_waits++;
                
                /* Update max (race possible but per-cpu minimizes it) */
                if (wait_time > s->max_wait_ns)
                    s->max_wait_ns = wait_time;
                
                /* Per-tier stats */
                if (tier < CAKE_TIER_MAX) {
                    s->total_wait_ns_tier[tier] += wait_time;
                    s->nr_waits_tier[tier]++;
                    if (wait_time > s->max_wait_ns_tier[tier])
                        s->max_wait_ns_tier[tier] = wait_time;
                }
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
         */
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
                
                if (enable_stats) {
                    struct cake_stats *s = get_local_stats();
                    if (s) {
                        s->nr_wait_demotions++;
                        if (tier < CAKE_TIER_MAX)
                            s->nr_wait_demotions_tier[tier]++;
                    }
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


    /* Update global vtime */
    if (time_before(vtime_now, p->scx.dsq_vtime))
        vtime_now = p->scx.dsq_vtime;
}

/*
 * Task is stopping (yielding or being preempted)
 */
void BPF_STRUCT_OPS(cake_stopping, struct task_struct *p, bool runnable)
{
    struct cake_task_ctx *tctx;
    u64 now = bpf_ktime_get_ns();
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

    /* Charge vtime based on runtime and weight */
    /* Charge vtime based on runtime and weight */
    if (likely(p->scx.weight == 100)) {
        /* Optimization: Standard weight (nice 0) - avoid division */
        p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice);
    } else {
        /* 
         * OPTIMIZATION: Bitwise vtime scaling
         * Use static_prio to lookup pre-calculated inverse weight.
         * Maps pure integer math (>> 16) instead of division.
         */
        int idx = p->static_prio - 100;
        
        /* Clamp index to valid range (0-39) */
        if (idx < 0) idx = 0;
        else if (idx > 39) idx = 39;
        
        u64 inv_weight = sched_prio_to_w_inv[idx];
        u64 delta = SCX_SLICE_DFL - p->scx.slice;
        
        p->scx.dsq_vtime += (delta * inv_weight) >> 16;
    }

    /* Update deficit (us) */
    /* deficit is u16 (us), runtime is u64 (ns). Convert runtime to us (>> 10) */
    u32 runtime_us = (u32)(runtime >> 10);
    if (runtime_us < tctx->deficit_us)
        tctx->deficit_us -= (u16)runtime_us;
    else
        tctx->deficit_us = 0;
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
    u32 key = 0;
    u32 *val = bpf_map_lookup_elem(&idle_stats_map, &key);
    if (val) {
        if (idle)
            __sync_fetch_and_add(val, 1);
        else
            __sync_fetch_and_add(val, -1);
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
        u32 runtime_32 = (u32)bpf_ktime_get_ns() - tctx->last_run_at;
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
    /* Initialize task's vtime to current global vtime */
    p->scx.dsq_vtime = vtime_now;
}

/*
 * Initialize the scheduler
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(cake_init)
{
    s32 ret;

    /* BITWISE OPTIMIZATION: >> 10 (~ / 1024) instead of / 1000 */
    cached_threshold_ns = (quantum_ns * sparse_threshold) >> 10;

    /* Initialize Idle Count (Correctness Sync) */
    u32 nr_cpus = scx_bpf_nr_cpu_ids();
    u32 count = 0;
    /* Loop bounded for verifier (max 1024 CPUs) */
    for (s32 i = 0; i < 1024; i++) {
        if (i >= nr_cpus) break;
        bpf_rcu_read_lock();
        struct task_struct *p = scx_bpf_cpu_curr(i);
        if (p && p->pid == 0) count++;
        bpf_rcu_read_unlock();
    }
    u32 key = 0;
    bpf_map_update_elem(&idle_stats_map, &key, &count, BPF_ANY);

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

    ret = scx_bpf_create_dsq(GAMING_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(INTERACTIVE_DSQ, -1);
    if (ret < 0)
        return ret;

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
               .init           = (void *)cake_init,
               .exit           = (void *)cake_exit,
               .flags          = SCX_OPS_KEEP_BUILTIN_IDLE,
               .name           = "cake");
