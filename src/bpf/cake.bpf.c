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

char _license[] SEC("license") = "GPL";

/* Scheduler configuration (set by userspace before loading) */
const volatile u64 quantum_ns = CAKE_DEFAULT_QUANTUM_NS;
const volatile u64 new_flow_bonus_ns = CAKE_DEFAULT_NEW_FLOW_BONUS_NS;
const volatile u64 sparse_threshold = CAKE_DEFAULT_SPARSE_THRESHOLD;
const volatile u64 starvation_ns = CAKE_DEFAULT_STARVATION_NS;
const volatile bool debug = false;
const volatile bool enable_stats = false;  /* Set to true when --verbose is used */

/*
 * Global statistics
 */
struct cake_stats stats = {};

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
static const u32 tier_multiplier[CAKE_TIER_MAX] = {
    70,   /* Critical Latency: 0.7x - smallest slice, input handlers */
    80,   /* Realtime:    0.8x - very responsive */
    90,   /* Critical:    0.9x */
    100,  /* Gaming:      1.0x (baseline) */
    110,  /* Interactive: 1.1x */
    120,  /* Batch:       1.2x */
    130,  /* Background:  1.3x - largest slice, less switching */
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
static const u64 wait_budget[CAKE_TIER_MAX] = {
    WAIT_BUDGET_CRITICAL_LATENCY, /* Tier 0: Critical Latency - 100µs */
    WAIT_BUDGET_REALTIME,    /* Tier 1: Realtime - 750µs */
    WAIT_BUDGET_CRITICAL,    /* Tier 2: Critical - 2ms */
    WAIT_BUDGET_GAMING,      /* Tier 3: Gaming - 4ms */
    WAIT_BUDGET_INTERACTIVE, /* Tier 4: Interactive - 8ms */
    WAIT_BUDGET_BATCH,       /* Tier 5: Batch - 20ms */
    0,                       /* Tier 6: Background - no limit */
};

/*
 * Per-tier starvation thresholds (nanoseconds)
 * Safety net: force preemption if task runs longer than its tier allows.
 * 
 * Formula: Starvation = 2x Wait Budget (except Background which has generous limit)
 */
#define STARVATION_CRITICAL_LATENCY 200000   /* 200µs - 2x wait budget */
#define STARVATION_REALTIME     1500000    /* 1.5ms - 2x wait budget */
#define STARVATION_CRITICAL     4000000    /* 4ms - 2x wait budget */
#define STARVATION_GAMING       8000000    /* 8ms - 2x wait budget */
#define STARVATION_INTERACTIVE  16000000   /* 16ms - 2x wait budget */
#define STARVATION_BATCH        40000000   /* 40ms - 2x wait budget */
#define STARVATION_BACKGROUND   100000000  /* 100ms - generous for bulk work */

/* Array for O(1) starvation threshold lookup (avoids switch in hot path) */
static const u64 starvation_threshold[CAKE_TIER_MAX] = {
    STARVATION_CRITICAL_LATENCY, /* Tier 0: Critical Latency - 200µs */
    STARVATION_REALTIME,     /* Tier 1: Realtime - 1.5ms */
    STARVATION_CRITICAL,     /* Tier 2: Critical - 4ms */
    STARVATION_GAMING,       /* Tier 3: Gaming - 8ms */
    STARVATION_INTERACTIVE,  /* Tier 4: Interactive - 16ms */
    STARVATION_BATCH,        /* Tier 5: Batch - 40ms */
    STARVATION_BACKGROUND,   /* Tier 6: Background - 100ms */
};

/*
 * Score-to-tier lookup table for O(1) tier classification
 * Eliminates cascading if-else branches in hot path
 * Note: score=100 may be overridden by latency gates in classify_tier()
 */
static const u8 score_to_tier[101] = {
    /* 0-29: Background */
    [0 ... 29] = CAKE_TIER_BACKGROUND,
    /* 30-49: Batch */
    [30 ... 49] = CAKE_TIER_BATCH,
    /* 50-69: Interactive */
    [50 ... 69] = CAKE_TIER_INTERACTIVE,
    /* 70-89: Gaming */
    [70 ... 89] = CAKE_TIER_GAMING,
    /* 90-100: Critical (score=100 may be promoted by latency gates) */
    [90 ... 100] = CAKE_TIER_CRITICAL,
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

    ctx->deficit = quantum_ns + new_flow_bonus_ns;
    ctx->last_run_at = 0;
    ctx->total_runtime = 0;
    ctx->last_wake_at = 0;
    ctx->wake_count = 0;
    ctx->run_count = 0;
    ctx->sparse_score = 50; /* Start neutral */
    ctx->tier = CAKE_TIER_INTERACTIVE;
    ctx->flags = CAKE_FLOW_NEW;

    return ctx;
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
    u32 score = tctx->sparse_score;
    
    /* Special latency gates for score=100 tasks */
    if (score == 100 && tctx->run_count > 10) {
        /* Gate 1: CritLatency - avg < 50µs */
        if (tctx->total_runtime < tctx->run_count * 50000) {
            return CAKE_TIER_CRITICAL_LATENCY;
        }
        /* Gate 2: Realtime - avg < 500µs */
        if (tctx->total_runtime < tctx->run_count * 500000) {
            return CAKE_TIER_REALTIME;
        }
        /* Falls through to lookup table -> Critical */
    }
    
    /* O(1) lookup table for all tiers (90-100 maps to Critical) */
    if (score > 100)
        score = 100;
    return score_to_tier[score];
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
    u32 old_score = tctx->sparse_score;
    u32 score = old_score;
    u64 threshold_ns = cached_threshold_ns;

    if (runtime_ns < threshold_ns) {
        /* Short runtime = sparse, increase score (slower gain) */
        score += 4;
        if (score > 100) score = 100;
    } else {
        /* Long runtime = bulk, decrease score (faster drop) */
        if (score >= 6) score -= 6;
        else score = 0;
    }

    tctx->sparse_score = score;

    /* Track tier transitions at Gaming threshold (most important boundary) */
    bool was_gaming_or_above = old_score >= THRESHOLD_GAMING;
    bool is_gaming_or_above = score >= THRESHOLD_GAMING;
    
    if (!was_gaming_or_above && is_gaming_or_above) {
        /* Promoted to Gaming or Critical tier */
        tctx->tier_switches++;
        if (enable_stats)
            __sync_fetch_and_add(&stats.nr_sparse_promotions, 1);
    } else if (was_gaming_or_above && !is_gaming_or_above) {
        /* Demoted from Gaming/Critical tier */
        tctx->tier_switches++;
        if (enable_stats)
            __sync_fetch_and_add(&stats.nr_sparse_demotions, 1);
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

    /* Track wake time for wait budget calculations */
    tctx->last_wake_at = bpf_ktime_get_ns();
    tctx->wake_count++;

    /* Try to find an idle CPU, prefer same LLC */
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

    /* Direct dispatch if idle CPU found - bypasses DSQ entirely */
    if (is_idle) {
        /* Use SCX_ENQ_LAST to skip redundant enqueue call - saves 5-20µs */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, quantum_ns, SCX_ENQ_LAST);
        if (enable_stats)
            __sync_fetch_and_add(&stats.nr_new_flow_dispatches, 1);
        return cpu;  /* Critical: return early to avoid cake_enqueue */
    }

    /* 
     * Preemption Injection
     * If we didn't find an idle CPU, but this is a latency-sensitive task,
     * check if the target CPU is running something low priority.
     */
    if (tctx->sparse_score >= 100) {
        struct task_struct *curr = scx_bpf_cpu_curr(cpu);
        struct cake_task_ctx *curr_ctx = get_task_ctx(curr);
        
        /* If current task is Batch or Background, it's a valid victim */
        if (curr_ctx && curr_ctx->tier >= CAKE_TIER_BATCH) {
            u32 cpu_key = cpu;
            u64 *last_preempt;
            u64 now = bpf_ktime_get_ns();
            
            last_preempt = bpf_map_lookup_elem(&preempt_cooldown, &cpu_key);
            if (last_preempt) {
                /* Rate limit: 50µs cooldown to prevent interrupt storms */
                if (now - *last_preempt > 50000) {
                    *last_preempt = now;
                    scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
                    
                    if (enable_stats)
                        __sync_fetch_and_add(&stats.nr_input_preempts, 1);
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
    tctx->tier = tier;

    /* Track if this is a wakeup (new flow) or preemption (old flow) */
    if (enable_stats) {
        if (enq_flags & SCX_ENQ_WAKEUP) {
            __sync_fetch_and_add(&stats.nr_new_flow_dispatches, 1);
        } else {
            __sync_fetch_and_add(&stats.nr_old_flow_dispatches, 1);
        }

        /* 
         * Bound tier for stats array access using bitmask
         * BPF verifier requires provably bounded indices
         * tier & 0x7 ensures max value is 7 (CAKE_TIER_MAX-1 = 6)
         */
        u8 bounded_tier = tier & 0x7;
        if (bounded_tier < CAKE_TIER_MAX)
            __sync_fetch_and_add(&stats.nr_tier_dispatches[bounded_tier], 1);
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
    switch (tier) {
        case CAKE_TIER_CRITICAL_LATENCY:
            dsq_id = CRITICAL_LATENCY_DSQ;
            break;
        case CAKE_TIER_REALTIME:
            dsq_id = REALTIME_DSQ;
            break;
        case CAKE_TIER_CRITICAL:
            dsq_id = CRITICAL_DSQ;
            break;
        case CAKE_TIER_GAMING:
            dsq_id = GAMING_DSQ;
            break;
        case CAKE_TIER_INTERACTIVE:
            dsq_id = INTERACTIVE_DSQ;
            break;
        case CAKE_TIER_BATCH:
            dsq_id = BATCH_DSQ;
            break;
        case CAKE_TIER_BACKGROUND:
            dsq_id = BACKGROUND_DSQ;
            break;
        default:
            dsq_id = INTERACTIVE_DSQ;
            break;
    }

    /*
     * OPTIMIZATION: New flow bonus via slice adjustment
     * New flows have higher deficit (quantum + bonus), so give them
     * a longer initial slice. This effectively prioritizes them without
     * needing vtime/PRIQ ordering.
     */
    u64 slice = quantum_ns;
    if (tctx->deficit > quantum_ns) {
        /* New flow with bonus - give extra slice time */
        slice = tctx->deficit;
    }

    /*
     * Apply tier-based quantum multiplier
     * Higher tiers get SMALLER slices (more responsive)
     * Lower tiers get LARGER slices (less context switching)
     */
    if (tier < CAKE_TIER_MAX) {
        slice = (slice * tier_multiplier[tier]) / 100;
    }

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
     * Using low bits of timestamp avoids global counter contention.
     * 0xF mask on shifted timestamp gives ~1/16 probability.
     */
    if (((bpf_ktime_get_ns() >> 10) & 0xF) == 0) {
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

    /* Track wait time: how long from wake to actually running */
    if (tctx->last_wake_at > 0) {
        u64 wait_time = now - tctx->last_wake_at;
        u8 tier = tctx->tier;
        
        /* Global and per-tier stats (only when stats enabled) */
        if (enable_stats) {
            __sync_fetch_and_add(&stats.total_wait_ns, wait_time);
            __sync_fetch_and_add(&stats.nr_waits, 1);
            
            /* Update max (race possible but acceptable for stats) */
            if (wait_time > stats.max_wait_ns)
                stats.max_wait_ns = wait_time;
            
            /* Per-tier stats */
            if (tier < CAKE_TIER_MAX) {
                __sync_fetch_and_add(&stats.total_wait_ns_tier[tier], wait_time);
                __sync_fetch_and_add(&stats.nr_waits_tier[tier], 1);
                if (wait_time > stats.max_wait_ns_tier[tier])
                    stats.max_wait_ns_tier[tier] = wait_time;
            }
        }
        
        /*
         * CAKE-style wait budget enforcement with percentage-based demotion
         * 
         * Instead of consecutive violations, we track violation ratio:
         * If > 25% of waits exceed budget, demote to lower tier.
         * Requires minimum 10 samples to avoid premature demotion.
         *
         * OPTIMIZATION: O(1) array lookup instead of switch statement.
         */
        u64 budget = (tier < CAKE_TIER_MAX) ? wait_budget[tier] : 0;
        
        /* Always increment check count for ratio calculation */
        tctx->wait_checks++;
        
        if (budget > 0 && wait_time > budget) {
            /* Violation - task waited too long for its tier */
            tctx->wait_violations++;
        }
        
        /*
         * Check violation ratio after minimum 10 samples
         * Demote if > 25% violations (violations * 4 > checks)
         * Using multiplication to avoid division in hot path
         */
        if (tctx->wait_checks >= 10 && tier < CAKE_TIER_BACKGROUND) {
            if (tctx->wait_violations * 4 > tctx->wait_checks) {
                /* > 25% violation rate - demote to next lower tier */
                tctx->tier = tier + 1;
                tctx->sparse_score = (tctx->sparse_score > 30) ? 
                                     tctx->sparse_score - 30 : 0;
                /* Reset counters for new tier */
                tctx->wait_violations = 0;
                tctx->wait_checks = 0;
                tctx->tier_switches++;
                if (enable_stats) {
                    __sync_fetch_and_add(&stats.nr_wait_demotions, 1);
                    if (tier < CAKE_TIER_MAX)
                        __sync_fetch_and_add(&stats.nr_wait_demotions_tier[tier], 1);
                }
            }
        }
        
        /* Clear last_wake_at to prevent double-counting if task runs again */
        tctx->last_wake_at = 0;
    }

    tctx->last_run_at = now;
    tctx->run_count++;

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

    runtime = now - tctx->last_run_at;
    tctx->total_runtime += runtime;

    /* Update sparse score - this determines Gaming vs Normal DSQ */
    update_sparse_score(tctx, runtime);

    /* Charge vtime based on runtime and weight */
    if (likely(p->scx.weight == 100)) {
        /* Optimization: Standard weight (nice 0) - avoid division */
        p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice);
    } else {
        p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;
    }

    /* Update deficit */
    if (runtime < tctx->deficit)
        tctx->deficit -= runtime;
    else
        tctx->deficit = 0;
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
void BPF_STRUCT_OPS(cake_tick, struct task_struct *p)
{
    struct cake_task_ctx *tctx;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

    /* Check for starvation using tier-specific threshold */
    if (tctx->last_run_at > 0) {
        u64 runtime = bpf_ktime_get_ns() - tctx->last_run_at;
        u8 tier = tctx->tier;
        
        /* O(1) threshold lookup via array (faster than switch) */
        u64 threshold = (tier < CAKE_TIER_MAX) ? 
                        starvation_threshold[tier] : STARVATION_BACKGROUND;
        
        if (runtime > threshold) {
            /* Force preemption - task exceeded its tier's starvation limit */
            scx_bpf_kick_cpu(scx_bpf_task_cpu(p), SCX_KICK_PREEMPT);
            
            /* Track per-tier starvation preempts */
            if (enable_stats && tier < CAKE_TIER_MAX)
                __sync_fetch_and_add(&stats.nr_starvation_preempts_tier[tier], 1);
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

    /* Precompute sparse threshold (avoid division in hot path) */
    cached_threshold_ns = (quantum_ns * sparse_threshold) / 1000;

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
               .tick           = (void *)cake_tick,
               .enable         = (void *)cake_enable,
               .init           = (void *)cake_init,
               .exit           = (void *)cake_exit,
               .name           = "cake");
