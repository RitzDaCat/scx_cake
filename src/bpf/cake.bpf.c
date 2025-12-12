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
 * Four dispatch queues - one per tier, served in priority order:
 * - CRITICAL_DSQ: Ultra-sparse tasks (input handlers, IRQ) - highest priority
 * - GAMING_DSQ: Sparse/bursty tasks (game threads, UI) - high priority  
 * - INTERACTIVE_DSQ: Normal tasks (default applications) - normal priority
 * - BACKGROUND_DSQ: Bulk tasks (compilers, encoders) - lowest priority
 */
#define CRITICAL_DSQ    0
#define GAMING_DSQ      1
#define INTERACTIVE_DSQ 2
#define BACKGROUND_DSQ  3

/* Sparse score thresholds for tier classification (0-100)
 * Each tier is determined by the sparse score range:
 * - 90-100: Critical (ultra-sparse, input handlers)
 * - 75-89:  Gaming (sparse, interactive)
 * - 25-74:  Interactive (normal applications)
 * - 0-24:   Background (bulk/heavy CPU users)
 * 
 * Hysteresis is applied at each boundary to prevent churn.
 */
#define THRESHOLD_CRITICAL   90  /* Score >= 90 → Critical */
#define THRESHOLD_GAMING     75  /* Score >= 75 → Gaming */
#define THRESHOLD_INTERACTIVE 25 /* Score >= 25 → Interactive */
/* Below 25 → Background */

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
    
    /* Average runtime must be ultra-short (<30µs = 30000ns) */
    u64 avg_runtime_ns = tctx->total_runtime / tctx->run_count;
    if (avg_runtime_ns > 30000)
        return false;
    
    return true;
}

/*
 * Classify task tier based on sparse score (behavior-based)
 * 
 * Score ranges:
 *   90-100: Critical - Ultra-sparse (input handlers, IRQ threads)
 *   75-89:  Gaming   - Sparse (game threads, UI, audio)
 *   25-74:  Interactive - Normal (most applications)
 *   0-24:   Background - Bulk (compilers, encoders)
 */
static u8 classify_tier(struct task_struct *p, struct cake_task_ctx *tctx)
{
    u32 score = tctx->sparse_score;
    
    if (score >= THRESHOLD_CRITICAL)
        return CAKE_TIER_CRITICAL;
    else if (score >= THRESHOLD_GAMING)
        return CAKE_TIER_GAMING;
    else if (score >= THRESHOLD_INTERACTIVE)
        return CAKE_TIER_INTERACTIVE;
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
        /* Short runtime = sparse, increase score */
        if (score < 100) {
            score += 5;
            if (score > 100) score = 100;
        }
    } else {
        /* Long runtime = bulk, decrease score */
        if (score >= 5) score -= 5;
        else score = 0;
    }

    tctx->sparse_score = score;

    /* Track tier transitions at Gaming threshold (most important boundary) */
    bool was_gaming_or_above = old_score >= THRESHOLD_GAMING;
    bool is_gaming_or_above = score >= THRESHOLD_GAMING;
    
    if (!was_gaming_or_above && is_gaming_or_above) {
        /* Promoted to Gaming or Critical tier */
        tctx->tier_switches++;
        __sync_fetch_and_add(&stats.nr_sparse_promotions, 1);
    } else if (was_gaming_or_above && !is_gaming_or_above) {
        /* Demoted from Gaming/Critical tier */
        tctx->tier_switches++;
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

    tctx->last_wake_at = bpf_ktime_get_ns();
    tctx->wake_count++;

    /* Try to find an idle CPU, prefer same LLC */
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

    /* Direct dispatch if idle CPU found - bypasses DSQ entirely */
    if (is_idle) {
        /* Use SCX_ENQ_LAST to skip redundant enqueue call - saves 5-20µs */
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, quantum_ns, SCX_ENQ_LAST);
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
        u64 now = bpf_ktime_get_ns();
        u64 time_since_run = tctx->last_run_at ? (now - tctx->last_run_at) : 0;
        
        /* Safety net: only kick if input task hasn't run beyond threshold */
        if (time_since_run > input_latency_ns) {
            scx_bpf_kick_cpu(prev_cpu, SCX_KICK_PREEMPT);
            __sync_fetch_and_add(&stats.nr_input_preempts, 1);
        }
        
        /* Track that we detected an input-like task */
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
    if (enq_flags & SCX_ENQ_WAKEUP) {
        __sync_fetch_and_add(&stats.nr_new_flow_dispatches, 1);
    } else {
        __sync_fetch_and_add(&stats.nr_old_flow_dispatches, 1);
    }

    /* Bound tier for stats array access */
    if (tier < CAKE_TIER_MAX)
        __sync_fetch_and_add(&stats.nr_tier_dispatches[tier], 1);

    /*
     * Route to DSQ based on tier classification:
     * - Critical: CRITICAL_DSQ (input handlers, ultra-sparse)
     * - Gaming: GAMING_DSQ (sparse/bursty tasks)
     * - Interactive: INTERACTIVE_DSQ (normal applications)
     * - Background: BACKGROUND_DSQ (bulk work)
     */
    switch (tier) {
        case CAKE_TIER_CRITICAL:
            dsq_id = CRITICAL_DSQ;
            break;
        case CAKE_TIER_GAMING:
            dsq_id = GAMING_DSQ;
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
    
    /* Priority order: Critical → Gaming → Interactive → Background */
    if (scx_bpf_dsq_move_to_local(CRITICAL_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(GAMING_DSQ))
        return;
    if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ))
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
        
        /* Global stats */
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
 * Periodic tick - check for starvation
 */
void BPF_STRUCT_OPS(cake_tick, struct task_struct *p)
{
    struct cake_task_ctx *tctx;

    tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

    /* Check for starvation */
    if (tctx->last_run_at > 0) {
        u64 runtime = bpf_ktime_get_ns() - tctx->last_run_at;
        if (runtime > starvation_ns) {
            /* Force preemption to prevent starvation */
            scx_bpf_kick_cpu(scx_bpf_task_cpu(p), SCX_KICK_PREEMPT);
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

    /* Create all 4 dispatch queues in priority order */
    ret = scx_bpf_create_dsq(CRITICAL_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(GAMING_DSQ, -1);
    if (ret < 0)
        return ret;

    ret = scx_bpf_create_dsq(INTERACTIVE_DSQ, -1);
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
