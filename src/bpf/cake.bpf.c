// SPDX-License-Identifier: GPL-2.0
/*
 * scx_cake - A sched_ext scheduler applying CAKE bufferbloat concepts
 *
 * This scheduler adapts CAKE's DRR++ (Deficit Round Robin++) algorithm
 * for CPU scheduling, providing low-latency scheduling for gaming and
 * interactive workloads.
 *
 * Key concepts from CAKE adapted here:
 * - New-flow vs old-flow: Waking tasks get priority over running/preempted tasks
 * - Sparse flow detection: Low-CPU tasks (like gaming) get latency priority
 * - Deficit scheduling: Fair share based on runtime accounting
 * - Priority tiers: Voice > Gaming > Best Effort > Background
 */

#include <scx/common.bpf.h>
#include "intf.h"

char _license[] SEC("license") = "GPL";

/* Scheduler configuration (set by userspace before loading) */
const volatile u64 quantum_ns = CAKE_DEFAULT_QUANTUM_NS;
const volatile u64 new_flow_bonus_ns = CAKE_DEFAULT_NEW_FLOW_BONUS_NS;
const volatile u64 sparse_threshold = CAKE_DEFAULT_SPARSE_THRESHOLD;
const volatile u64 starvation_ns = CAKE_DEFAULT_STARVATION_NS;
const volatile bool debug = false;

/*
 * Global statistics
 */
struct cake_stats stats = {};

/* User exit info for graceful scheduler exit */
UEI_DEFINE(uei);

/* Global vtime for fair scheduling */
static u64 vtime_now;

/*
 * We use a single shared DSQ for simplicity, similar to scx_simple.
 * This ensures the scheduler works before adding complexity.
 */
#define SHARED_DSQ 0

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
    ctx->wake_count = 0;
    ctx->run_count = 0;
    ctx->sparse_score = 50; /* Start neutral */
    ctx->tier = CAKE_TIER_BESTEFFORT;
    ctx->flags = CAKE_FLOW_NEW;

    return ctx;
}

/*
 * Classify task tier based on nice value and sparse score
 */
static u8 classify_tier(struct task_struct *p, struct cake_task_ctx *tctx)
{
    s32 nice = p->static_prio - 120; /* Convert priority to nice value */

    /* Sparse flows (interactive) get boosted to Gaming tier */
    if (tctx->sparse_score >= 70)
        return CAKE_TIER_GAMING;

    /* Use nice value for tier assignment */
    if (nice <= -10)
        return CAKE_TIER_VOICE;
    else if (nice < 0)
        return CAKE_TIER_GAMING;
    else if (nice >= 10)
        return CAKE_TIER_BACKGROUND;
    else
        return CAKE_TIER_BESTEFFORT;
}

/*
 * Update sparse score based on runtime behavior
 */
static void update_sparse_score(struct cake_task_ctx *tctx, u64 runtime_ns)
{
    u32 score = tctx->sparse_score;
    u64 threshold_ns = (quantum_ns * sparse_threshold) / 1000;

    if (runtime_ns < threshold_ns) {
        /* Short runtime = sparse, increase score */
        if (score < 100) {
            score += 5;
            if (score > 100) score = 100;
            if (score >= 70 && tctx->sparse_score < 70)
                __sync_fetch_and_add(&stats.nr_sparse_promotions, 1);
        }
    } else {
        /* Long runtime = bulk, decrease score */
        if (score > 0) {
            if (score >= 5) score -= 5;
            else score = 0;
            if (score < 70 && tctx->sparse_score >= 70)
                __sync_fetch_and_add(&stats.nr_sparse_demotions, 1);
        }
    }

    tctx->sparse_score = score;
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
    if (tctx) {
        tctx->last_wake_at = bpf_ktime_get_ns();
        tctx->wake_count++;
    }

    /* Try to find an idle CPU, prefer same LLC */
    cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);

    /* Direct dispatch if idle CPU found */
    if (is_idle) {
        scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
        __sync_fetch_and_add(&stats.nr_new_flow_dispatches, 1);
    }

    return cpu;
}

/*
 * Enqueue task to the scheduler
 */
void BPF_STRUCT_OPS(cake_enqueue, struct task_struct *p, u64 enq_flags)
{
    struct cake_task_ctx *tctx;
    u8 tier;

    tctx = get_task_ctx(p);
    if (!tctx) {
        /* Fallback: dispatch to shared DSQ */
        scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
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

    /* Use FIFO dispatch to shared DSQ */
    scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
}

/*
 * Dispatch tasks to run on this CPU
 */
void BPF_STRUCT_OPS(cake_dispatch, s32 cpu, struct task_struct *prev)
{
    scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

/*
 * Task is starting to run
 */
void BPF_STRUCT_OPS(cake_running, struct task_struct *p)
{
    struct cake_task_ctx *tctx;

    tctx = get_task_ctx(p);
    if (!tctx)
        return;

    tctx->last_run_at = bpf_ktime_get_ns();
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
    if (!tctx || tctx->last_run_at == 0)
        return;

    runtime = now - tctx->last_run_at;
    tctx->total_runtime += runtime;

    /* Update sparse score */
    update_sparse_score(tctx, runtime);

    /* Charge vtime based on runtime and weight */
    p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;

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
    if (!tctx)
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
    /* Create a single shared DSQ (like scx_simple) */
    return scx_bpf_create_dsq(SHARED_DSQ, -1);
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
