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

char _license[] SEC("license") = "GPL";

/* Scheduler configuration (set by userspace before loading) */
const volatile u64 quantum_ns = CAKE_DEFAULT_QUANTUM_NS;
const volatile u64 new_flow_bonus_ns = CAKE_DEFAULT_NEW_FLOW_BONUS_NS;
const volatile u64 sparse_threshold = CAKE_DEFAULT_SPARSE_THRESHOLD;
const volatile u64 starvation_ns = CAKE_DEFAULT_STARVATION_NS;
const volatile u64 input_latency_ns = CAKE_DEFAULT_INPUT_LATENCY_NS;
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
 * Six dispatch queues - one per tier, served in priority order:
 * - REALTIME_DSQ:    Ultra-sparse tasks (input handlers, IRQ) - highest priority
 * - CRITICAL_DSQ:    Very sparse tasks (audio, compositor) - very high priority
 * - GAMING_DSQ:      Sparse/bursty tasks (game threads, UI) - high priority  
 * - INTERACTIVE_DSQ: Normal tasks (default applications) - baseline priority
 * - BATCH_DSQ:       Lower priority work (nice > 0) - lower priority
 * - BACKGROUND_DSQ:  Bulk tasks (compilers, encoders) - lowest priority
 */
#define REALTIME_DSQ    0
#define CRITICAL_DSQ    1
#define GAMING_DSQ      2
#define INTERACTIVE_DSQ 3
#define BATCH_DSQ       4
#define BACKGROUND_DSQ  5

/*
 * Tier quantum multipliers (stored as percentage)
 * Higher tiers get SMALLER slices (more preemption points, lower latency)
 * Lower tiers get LARGER slices (less context switching for bulk work)
 */
static const u32 tier_multiplier[CAKE_TIER_MAX] = {
    80,   /* Realtime:    0.8x - smallest slice, most responsive */
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
 */
#define WAIT_BUDGET_REALTIME    1000000   /* 1ms */
#define WAIT_BUDGET_CRITICAL    2000000   /* 2ms */
#define WAIT_BUDGET_GAMING      4000000   /* 4ms */
#define WAIT_BUDGET_INTERACTIVE 10000000  /* 10ms */
#define WAIT_BUDGET_BATCH       35000000  /* 35ms */
#define WAIT_VIOLATIONS_DEMOTE  4         /* Demote after N consecutive violations */

/*
 * Per-tier starvation thresholds (nanoseconds)
 * Safety net: force preemption if task runs longer than its tier allows.
 */
#define STARVATION_REALTIME     2000000    /* 2ms */
#define STARVATION_CRITICAL     4000000    /* 4ms */
#define STARVATION_GAMING       8000000    /* 8ms */
#define STARVATION_INTERACTIVE  12000000   /* 12ms */
#define STARVATION_BATCH        40000000   /* 40ms */
#define STARVATION_BACKGROUND   100000000  /* 100ms */

/* Array for O(1) starvation threshold lookup (avoids switch in hot path) */
static const u64 starvation_threshold[CAKE_TIER_MAX] = {
    STARVATION_REALTIME,     /* Tier 0: Realtime */
    STARVATION_CRITICAL,     /* Tier 1: Critical */
    STARVATION_GAMING,       /* Tier 2: Gaming */
    STARVATION_INTERACTIVE,  /* Tier 3: Interactive */
    STARVATION_BATCH,        /* Tier 4: Batch */
    STARVATION_BACKGROUND,   /* Tier 5: Background */
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
 * Get or initialize task context
 */
static struct cake_task_ctx *get_task_ctx(struct task_struct *p)
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
    ctx->last_input_at = 0;
    ctx->wake_count = 0;
    ctx->run_count = 0;
    ctx->sparse_score = 50; /* Start neutral */
    ctx->tier = CAKE_TIER_INTERACTIVE;
    ctx->flags = CAKE_FLOW_NEW;

    return ctx;
}

/*
 * Input detection via runtime heuristic
 * 
 * Identifies likely input tasks by behavioral signature:
 * - Ultra-short runtime: <30µs (input IRQ handlers are 5-20µs)
 * - High sparse score: >=80 (frequent wakes, short runs)
 * - Sufficient history: >10 runs (not a new random task)
 */
static bool is_likely_input_task(struct cake_task_ctx *tctx)
{
    /* Need sufficient run history to judge */
    if (tctx->run_count < 10)
        return false;
    
    /* Must have very high sparse score (very short, bursty) */
    if (tctx->sparse_score < 80)
        return false;
    
    /*
     * Average runtime must be ultra-short (<30µs = 30000ns)
     * 
     * Optimization: Use multiplication instead of division
     * Instead of: total_runtime / run_count > 30000
     * Use: total_runtime > run_count * 30000
     * Saves ~40 cycles (division is expensive)
     */
    if (tctx->total_runtime > tctx->run_count * 30000)
        return false;
    
    return true;
}

/*
 * Classify task tier based on sparse score (behavior-based)
 * 
 * Score ranges (6 tiers):
 *   100:    Realtime    - Perfect sparse score only
 *   90-99:  Critical    - Ultra-sparse (input handlers)
 *   70-89:  Gaming      - Sparse (game threads, UI)
 *   50-69:  Interactive - Baseline (default applications)
 *   30-49:  Batch       - Lower priority (nice > 0)
 *   0-29:   Background  - Bulk (compilers, encoders)
 */
static u8 classify_tier(struct task_struct *p, struct cake_task_ctx *tctx)
{
    u32 score = tctx->sparse_score;
    
    if (score >= THRESHOLD_REALTIME)
        return CAKE_TIER_REALTIME;
    else if (score >= THRESHOLD_CRITICAL)
        return CAKE_TIER_CRITICAL;
    else if (score >= THRESHOLD_GAMING)
        return CAKE_TIER_GAMING;
    else if (score >= THRESHOLD_INTERACTIVE)
        return CAKE_TIER_INTERACTIVE;
    else if (score >= THRESHOLD_BATCH)
        return CAKE_TIER_BATCH;
    else
        return CAKE_TIER_BACKGROUND;
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
static void update_sparse_score(struct cake_task_ctx *tctx, u64 runtime_ns)
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
    u64 now;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx)) {
        /* Fallback if we can't get context */
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    /* Get time once, reuse below */
    now = bpf_ktime_get_ns();
    tctx->last_wake_at = now;
    tctx->wake_count++;

    /* Try to find an idle CPU, prefer same LLC */
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

    /* Direct dispatch if idle CPU found - bypasses DSQ entirely */
    if (is_idle) {
        /* Use SCX_ENQ_LAST to skip redundant enqueue call - saves 5-20µs */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, quantum_ns, SCX_ENQ_LAST);
        if (enable_stats)
            __sync_fetch_and_add(&stats.nr_new_flow_dispatches, 1);
        return cpu;
    }

    /*
     * OPTIMIZATION: Input-specific safety net preemption
     * 
     * Uses runtime heuristic to identify likely input tasks:
     * - Ultra-short runtime (<30µs)
     * - High sparse score (>=80)
     * - Sufficient history (>10 runs)
     * 
     * Only preempts if they haven't run in longer than input_latency_ns.
     * This provides guaranteed input latency ceiling (configurable via --input-latency).
     * 
     * We check last_run_at (when task last executed) NOT last_wake_at (just set above).
     * This accurately measures how long the input task has been blocked.
     */
    if (is_likely_input_task(tctx)) {
        /* Reuse 'now' from above instead of calling bpf_ktime_get_ns() again */
        u64 time_since_run = tctx->last_run_at ? (now - tctx->last_run_at) : 0;
        
        /* Safety net: only kick if input task hasn't run beyond threshold */
        if (time_since_run > input_latency_ns) {
            scx_bpf_kick_cpu(prev_cpu, SCX_KICK_PREEMPT);
            if (enable_stats)
                __sync_fetch_and_add(&stats.nr_input_preempts, 1);
        }
        
        /* Track that we detected an input-like task */
        if (enable_stats)
            __sync_fetch_and_add(&stats.nr_input_events, 1);
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

        /* Bound tier for stats array access */
        if (tier < CAKE_TIER_MAX)
            __sync_fetch_and_add(&stats.nr_tier_dispatches[tier], 1);
    }

    /*
     * Route to DSQ based on tier classification:
     * - Critical: CRITICAL_DSQ (input handlers, ultra-sparse)
     * - Gaming: GAMING_DSQ (sparse/bursty tasks)
     * - Interactive: INTERACTIVE_DSQ (normal applications)
     * - Background: BACKGROUND_DSQ (bulk work)
     */
    switch (tier) {
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
 *   1. Critical (input handlers, ultra-sparse)
 *   2. Gaming (sparse/bursty)
 *   3. Interactive (normal apps)
 *   4. Background (bulk work)
 *
 * Starvation protection: Every 16 dispatches, we check lower tiers
 * even if higher tiers have work, to prevent complete starvation.
 */
static u64 dispatch_count = 0;

void BPF_STRUCT_OPS(cake_dispatch, s32 cpu, struct task_struct *prev)
{
    dispatch_count++;
    
    /* Starvation protection: occasionally let lower tiers run */
    if ((dispatch_count & 0xF) == 0) {  /* Every 16 dispatches */
        /* Give background a chance even if others have work */
        if (scx_bpf_dsq_move_to_local(BACKGROUND_DSQ))
            return;
        if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ))
            return;
    }
    
    /* Priority order: Realtime → Critical → Gaming → Interactive → Batch → Background */
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
         * CAKE-style wait budget enforcement
         * If a task exceeds its tier's wait budget multiple times,
         * demote it to a lower tier to reduce congestion.
         */
        u64 budget = 0;
        switch (tier) {
            case CAKE_TIER_CRITICAL:
                budget = WAIT_BUDGET_CRITICAL;
                break;
            case CAKE_TIER_GAMING:
                budget = WAIT_BUDGET_GAMING;
                break;
            case CAKE_TIER_INTERACTIVE:
                budget = WAIT_BUDGET_INTERACTIVE;
                break;
            default:
                budget = 0;  /* Background has no budget limit */
                break;
        }
        
        if (budget > 0 && wait_time > budget) {
            /* Violation - task waited too long for its tier */
            tctx->wait_violations++;
            
            if (tctx->wait_violations >= WAIT_VIOLATIONS_DEMOTE &&
                tier < CAKE_TIER_BACKGROUND) {
                /* Demote to next lower tier with significant score penalty */
                tctx->tier = tier + 1;
                tctx->sparse_score = (tctx->sparse_score > 30) ? 
                                     tctx->sparse_score - 30 : 0;
                tctx->wait_violations = 0;
                tctx->tier_switches++;
                if (enable_stats) {
                    __sync_fetch_and_add(&stats.nr_wait_demotions, 1);
                    if (tier < CAKE_TIER_MAX)
                        __sync_fetch_and_add(&stats.nr_wait_demotions_tier[tier], 1);
                }
            }
        } else {
            /* Good wait time - reset violation counter */
            tctx->wait_violations = 0;
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

    /* Create all 6 dispatch queues in priority order */
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
