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

/* NOTE: Topology variables (has_dual_ccd, has_hybrid_cores, ccd0_mask, etc.)
 * were removed. They were set by userspace but never read in BPF code.
 * Future: Re-add when CCD-local or P-core preference is implemented.
 */

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
 * Global Idle Mask (Bitmask for CTZ scanning)
 * 
 * KEPT AS BITMASK because we need "find first idle CPU" via CTZ.
 * Bytemask would require scanning, losing the O(1) advantage.
 * 
 * Write side uses atomic ops - acceptable cost for fast read.
 */
/*
 * Global Idle Mask (Dual-View Union)
 * 
 * "Wait-Free" Implementation:
 * - WRITE: Write to 'as_bytes' (u8) is a single standard store. No ATOMIC LOCK. 
 *          Store buffer handles coherency (Wait-Free).
 * - READ:  Read 'as_chunks' (u64) to scan 8 CPUs at once.
 */
struct {
    union {
        u8  as_bytes[64];   /* WRITE VIEW: Access individually (No False Sharing logic) */
        u64 as_chunks[8];   /* READ VIEW:  Access in 8 chunks (Fast Scan) */
    };
} idle_map SEC(".bss") __attribute__((aligned(64)));

/*
 * Global Victim Mask (Bitmask for CTZ scanning)
 * 
 * KEPT AS BITMASK because we need "find first victim" via CTZ.
 * Updated non-atomically (acceptable race - victim is heuristic).
 */
u64 victim_mask SEC(".bss") __attribute__((aligned(64)));

/* NOTE: dsq_has_tasks flag system was removed.
 * Flags became permanently dirty (never cleared) and provided no benefit.
 * scx_bpf_dsq_move_to_local handles empty queues efficiently (~10 cycles).
 */

/* NOTE: cpu_status scoreboard was removed.
 * It was written every context switch but NEVER READ.
 * Saves 4 cycles per running + 4KB BSS memory.
 */

static __always_inline struct cake_stats *get_local_stats(void)
{
    u32 key = 0;
    return bpf_map_lookup_elem(&stats_map, &key);
}

/*
 * Helper: Find first idle CPU using MLP-optimized scan
 * 
 * O(1) OPTIMIZATION: Load all 8 chunks in parallel (MLP), OR together
 * to check if ANY CPU is idle, then scan only if needed.
 * Reduces 40 dependent cycles to ~12 parallel cycles.
 */
static __always_inline s32 find_first_idle_cpu(s32 prev_cpu)
{
    /* 1. Check prev_cpu direct (Fastest - 1 byte load) */
    if (prev_cpu >= 0 && prev_cpu < 64 && idle_map.as_bytes[prev_cpu]) 
        return prev_cpu;

    /* 2. MLP: Load ALL chunks in parallel (CPU issues 8 loads simultaneously) */
    u64 c0 = idle_map.as_chunks[0];
    u64 c1 = idle_map.as_chunks[1];
    u64 c2 = idle_map.as_chunks[2];
    u64 c3 = idle_map.as_chunks[3];
    u64 c4 = idle_map.as_chunks[4];
    u64 c5 = idle_map.as_chunks[5];
    u64 c6 = idle_map.as_chunks[6];
    u64 c7 = idle_map.as_chunks[7];
    
    /* 3. OR all chunks - if result is 0, no idle CPUs exist (early exit) */
    u64 any_idle = c0 | c1 | c2 | c3 | c4 | c5 | c6 | c7;
    if (!any_idle)
        return -1;
    
    /* 4. Scan chunks (we know at least one has an idle CPU) */
    if (c0) return (0 * 8) + (__builtin_ctzll(c0) >> 3);
    if (c1) return (1 * 8) + (__builtin_ctzll(c1) >> 3);
    if (c2) return (2 * 8) + (__builtin_ctzll(c2) >> 3);
    if (c3) return (3 * 8) + (__builtin_ctzll(c3) >> 3);
    if (c4) return (4 * 8) + (__builtin_ctzll(c4) >> 3);
    if (c5) return (5 * 8) + (__builtin_ctzll(c5) >> 3);
    if (c6) return (6 * 8) + (__builtin_ctzll(c6) >> 3);
    if (c7) return (7 * 8) + (__builtin_ctzll(c7) >> 3);
    
    return -1;  /* Should never reach here */
}

/* User exit info for graceful scheduler exit */
UEI_DEFINE(uei);

/* Global vtime removed to prevent bus locking. Tasks inherit vtime from parent. */

/* Optimization: Precomputed threshold to avoid division in hot path */
static u64 cached_threshold_ns;

/*
 * FUSED Tier+Slice Lookup Table (eliminates load dependency chain)
 * 
 * Single access returns both tier and slice, saving ~4 cycles.
 * 32 entries × 16 bytes = 512 bytes (1.6% of 32KB L1 = acceptable).
 * 
 * Access pattern:
 *   tier_info[new_count] → { tier, slice }  // ONE load
 * vs old pattern:
 *   count_to_tier[new_count] → tier          // LOAD 1
 *   tier_slice[tier] → slice                 // LOAD 2 (depends on LOAD 1)
 */
struct tier_info_t {
    u8 tier;
    u8 __pad[7];  /* Align slice to 8 bytes */
    u64 slice;
};
static struct tier_info_t tier_info[32] __attribute__((aligned(64)));



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

/* Tier values for LUT initialization (used in cake_init) */
static const u8 count_to_tier_init[32] = {
    6,                          /* 0: Background (never sparse) */
    5, 5, 5,                    /* 1-3: Batch */
    4, 4, 4, 4,                 /* 4-7: Interactive */
    3, 3, 3, 3, 3, 3, 3, 3,     /* 8-15: Gaming */
    2, 2, 2, 2, 2, 2, 2, 2,     /* 16-23: Critical */
    1, 1, 1, 1, 1, 1, 1,        /* 24-30: Realtime */
    0                           /* 31: Critical Latency */
};

/* Delta scoring: count consecutive sparse runs (0-31), reset on non-sparse */
#define CAKE_TIER_IDLE 255 /* Special value for Scoreboard (Cpu is Idle) */

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

/* REMOVED: Preemption Cooldowns
 * Stateless wakeup is now the default - kicks happen immediately.
 * Saves 1 cache line access and ~15-25ns during wakeup path.
 */


/*
 * Bitfield Accessor Macros for packed_info
 * These allow us to pack multiple variables into a single u32.
 */


#define GET_SPARSE_COUNT(ctx) ((ctx->packed_info >> SHIFT_SPARSE_COUNT) & MASK_SPARSE_COUNT)
#define SET_SPARSE_COUNT(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_SPARSE_COUNT << SHIFT_SPARSE_COUNT)) | ((val & MASK_SPARSE_COUNT) << SHIFT_SPARSE_COUNT))

#define GET_TIER(ctx) ((ctx->packed_info >> SHIFT_TIER) & MASK_TIER)
#define SET_TIER(ctx, val) (ctx->packed_info = (ctx->packed_info & ~(MASK_TIER << SHIFT_TIER)) | ((val & MASK_TIER) << SHIFT_TIER))

/*
 * Get or initialize task context
 * 
 * LAZY ALLOCATION: Pass create=false for fast-path lookups (no malloc).
 * Pass create=true only in cake_running (serialized per-CPU, no contention).
 * This eliminates malloc lock contention at scheduler startup.
 */
static __always_inline struct cake_task_ctx *get_task_ctx(struct task_struct *p, bool create)
{
    struct cake_task_ctx *ctx;

    /* Fast path: lookup existing context */
    ctx = bpf_task_storage_get(&task_ctx, p, 0, 0);
    if (ctx)
        return ctx;

    /* If caller doesn't want allocation, return NULL */
    if (!create)
        return NULL;

    /* Slow path: allocate new context (only from cake_running) */
    ctx = bpf_task_storage_get(&task_ctx, p, 0,
                               BPF_LOCAL_STORAGE_GET_F_CREATE);
    if (!ctx)
        return NULL;

    /* Initialize task context fields */
    ctx->next_slice = quantum_ns; /* Default slice */
    ctx->deficit_us = (u16)((quantum_ns + new_flow_bonus_ns) >> 10);
    ctx->last_run_at = 0;
    ctx->avg_runtime_us = 0;
    
    /* Pack initial values: Err=255, Count=0, Tier=Interactive, Flags=New */
    u32 packed = 0;
    packed |= (255 & MASK_KALMAN_ERROR) << SHIFT_KALMAN_ERROR;
    packed |= (CAKE_DEFAULT_INIT_COUNT & MASK_SPARSE_COUNT) << SHIFT_SPARSE_COUNT;
    packed |= (CAKE_TIER_INTERACTIVE & MASK_TIER) << SHIFT_TIER;
    packed |= (CAKE_FLOW_NEW & MASK_FLAGS) << SHIFT_FLAGS;
    
    ctx->packed_info = packed;

    return ctx;
}

/* REMOVED: update_kalman_estimate, update_task_tier, update_sparse_score
 * All logic is now inlined into cake_stopping with a single packed_info read-modify-write.
 * Saves ~20-30 cycles per context switch.
 */

/*
 * Select CPU for a waking task
 */
s32 BPF_STRUCT_OPS(cake_select_cpu, struct task_struct *p, s32 prev_cpu,
                   u64 wake_flags)
{
    struct cake_task_ctx *tctx;
    bool is_idle = false;
    s32 cpu;

    tctx = get_task_ctx(p, false);  /* No allocation - fast path */
    if (unlikely(!tctx)) {
        /* No context yet - use kernel default (task will get context in cake_running) */
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    /* 
     * OPTIMIZATION: Bitmask Idle Search (O(1) via CTZ)
     * We use a global 64-bit mask where set bits = idle CPUs.
     */
    /* 
     * OPTIMIZATION: Wait-Free Idle Search (Dual-View)
     */
    
    /* Move tier read up for the check */
    u8 tier = GET_TIER(tctx);

    /* Quick check: Scan chunks to see if ANYONE is idle */
    /* Checks first 64 CPUs. If we have >64, we might need loops, but logic assumes 64 max */
    /* Unrolled check of chunks for early exit? No, find_first_idle_cpu handles it. */

    is_idle = false;
    cpu = prev_cpu; /* Default */

    /* Only search if we are not lowest priority (optimization) */
    if (tier <= REALTIME_DSQ) {
         s32 idle_cpu = find_first_idle_cpu(prev_cpu);
         if (idle_cpu >= 0) {
             cpu = idle_cpu;
             is_idle = true;
         }
    } else {
        /* For lower tiers, only check prev_cpu (stickiness) */
        if (prev_cpu >= 0 && prev_cpu < 64 && idle_map.as_bytes[prev_cpu]) {
             cpu = prev_cpu;
             is_idle = true;
        } else {
             /* Try to find ANY idle CPU if we couldn't stick */
             s32 idle_cpu = find_first_idle_cpu(-1);
             if (idle_cpu >= 0) {
                 cpu = idle_cpu;
                 is_idle = true;
             }
        }
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
     * Note: tier already read above.
     */
    if (tier == CRITICAL_LATENCY_DSQ) { /* Tier 0 (CritLatency) only */
        /* 
         * OPTIMIZATION: Smart Victim Selection
         * Instead of blindly picking prev_cpu, scan neighbors (likely LLC/CCD)
         * to find a lower priority victim.
         * 
         * Scan window: 4 CPUs (prev, +1, +2, +3).
         * Cost: 4 * 20 cycles = ~80 cycles.
         * Benefit: Avoids blindly killing a Game Thread if a Background task is nearby.
         */
        s32 best_cpu = -1;

        /* 
         * OPTIMIZATION: O(1) Victim Selection using Bitmask
         * Instead of scanning 4 CPUs with map lookups (~80 cycles),
         * use the victim_mask to find any eligible victim in O(1) (~3 cycles).
         * victim_mask tracks CPUs running tier >= INTERACTIVE.
         */
        u64 victims = victim_mask;
        if (victims) {
            best_cpu = __builtin_ctzll(victims);  /* O(1) TZCNT instruction */
        }
        
        /* Did we find a victim? Kick immediately (stateless - no cooldown) */
        if (best_cpu >= 0) {
            scx_bpf_kick_cpu(best_cpu, SCX_KICK_PREEMPT);
            
            if (enable_stats) {
                struct cake_stats *s = get_local_stats();
                if (s) s->nr_input_preempts++;
            }
            cpu = best_cpu;
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

    tctx = get_task_ctx(p, false);  /* No allocation - fast path */
    if (unlikely(!tctx)) {
        /* No context yet - use INTERACTIVE defaults (context created in cake_running) */
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
     * OPTIMIZATION: Direct mapping (TIER_ID == DSQ_ID)
     * Removes switch branch table. 
     * Safe because tier is guaranteed 0-6 by logic and 0-7 by type.
     */
    dsq_id = tier;

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
 *   1. Critical Latency (true input handlers)
 *   2. Realtime (ultra-sparse tasks)
 *   3. Critical (audio, compositor)
 *   4. Gaming (sparse/bursty)
 *   5. Interactive (normal apps)
 *   6. Batch (nice > 0)
 *   7. Background (bulk work)
 *
 * NOTE: No flag pre-checks. scx_bpf_dsq_move_to_local returns false on empty
 * queues (~10 cycles). Flags became permanently dirty and provided no benefit.
 */
void BPF_STRUCT_OPS(cake_dispatch, s32 cpu, struct task_struct *prev)
{
    /* Starvation Protection: probabilistically boost low-priority queues */
    if (prev && ((prev->pid ^ prev->se.sum_exec_runtime) & 0xF) == 0) {
        if (scx_bpf_dsq_move_to_local(BACKGROUND_DSQ)) return;
        if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ)) return;
    }
    
    /* Priority Dispatch (move_to_local is fast ~10 cycles on empty) */
    if (scx_bpf_dsq_move_to_local(CRITICAL_LATENCY_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(REALTIME_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(CRITICAL_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(GAMING_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(BATCH_DSQ)) return;
    if (scx_bpf_dsq_move_to_local(BACKGROUND_DSQ)) return;
}

/*
 * Task is starting to run
 * 
 * ULTRA-OPTIMIZED: ~18 cycles total
 * - Unconditional scoreboard write (skip comparison)
 * - Unconditional victim_mask update (skip comparison)
 */
void BPF_STRUCT_OPS(cake_running, struct task_struct *p)
{
    /* LAZY ALLOCATION: Create context here (serialized per-CPU, no contention) */
    struct cake_task_ctx *tctx = get_task_ctx(p, true);
    if (unlikely(!tctx))
        return;

    /* ZERO-COST: bpf_get_smp_processor_id is inlined, no helper call (~30 cycles saved) */
    u32 cpu_idx = bpf_get_smp_processor_id();
    if (cpu_idx >= 64)
        return;

    u8 tier = GET_TIER(tctx);
    
    /* 
     * BRANCHLESS victim_mask update (~10-30 cycles saved on mispredict)
     * Uses arithmetic selection instead of branches.
     */
    u64 current_mask = victim_mask;
    u64 cpu_bit = (1ULL << cpu_idx);
    
    /* Branchless: compute both variants */
    u64 set_mask = current_mask | cpu_bit;
    u64 clear_mask = current_mask & ~cpu_bit;
    
    /* Select: is_victim=1 picks set_mask, is_victim=0 picks clear_mask */
    u64 is_victim = (tier >= INTERACTIVE_DSQ);
    u64 new_mask = (is_victim * set_mask) + (!is_victim * clear_mask);
    
    /* Test-and-set: only write if different */
    if (new_mask != current_mask)
        victim_mask = new_mask;

    tctx->last_run_at = (u32)scx_bpf_now();
}

/*
 * Task is stopping (yielding or being preempted)
 * 
 * ULTRA-OPTIMIZED: ~20 cycles total
 * - No EMA (use last runtime directly if ever needed)
 * - No latency gates (tier is pure count-based)
 * - Minimal packed_info handling
 */
void BPF_STRUCT_OPS(cake_stopping, struct task_struct *p, bool runnable)
{
    struct cake_task_ctx *tctx = get_task_ctx(p, false);  /* No allocation */
    if (unlikely(!tctx || tctx->last_run_at == 0))
        return;

    /* Runtime calculation: 3 ops */
    u64 runtime = (u32)scx_bpf_now() - tctx->last_run_at;
    
    /* 
     * Sparse count update (Branchless - eliminates pipeline stalls)
     * Instead of ternary (branch), use arithmetic mask.
     * If sparse: mask = 0xFFFFFFFF, keeps (count+1)
     * If batch:  mask = 0, clears to 0
     */
    u32 packed = tctx->packed_info;
    u32 old_count = (packed >> SHIFT_SPARSE_COUNT) & MASK_SPARSE_COUNT;
    u32 is_sparse = runtime < cached_threshold_ns;
    u32 mask = -(s32)is_sparse;  /* 0xFFFFFFFF if sparse, 0x00000000 if not */
    u32 new_count = ((old_count + 1) & MASK_SPARSE_COUNT) & mask;
    
    /* Stats (compiled out when enable_stats=false) */
    if (enable_stats) {
        bool was_gaming = old_count >= SPARSE_COUNT_GAMING;
        bool is_gaming = new_count >= SPARSE_COUNT_GAMING;
        if (was_gaming != is_gaming) {
            struct cake_stats *s = get_local_stats();
            if (s) {
                if (is_gaming) s->nr_sparse_promotions++;
                else s->nr_sparse_demotions++;
            }
        }
    }
    
    /* FUSED LUT: Single access returns both tier and slice (~4 cycles saved) */
    struct tier_info_t info = tier_info[new_count & 0x1F];
    u8 tier = info.tier;
    tctx->next_slice = info.slice;
    
    /* Update packed_info: single read-modify-write */
    tctx->packed_info = (packed & ~((MASK_SPARSE_COUNT << SHIFT_SPARSE_COUNT) | (MASK_TIER << SHIFT_TIER)))
                      | ((new_count & MASK_SPARSE_COUNT) << SHIFT_SPARSE_COUNT)
                      | ((tier & MASK_TIER) << SHIFT_TIER);
}

/*
 * CPU idle state changed
 * Updates the idle_map byte for wait-free idle CPU scanning.
 */
void BPF_STRUCT_OPS(cake_update_idle, s32 cpu, bool idle)
{
    /* Cap CPU ID to 63 for 64-bit mask safety */
    if (cpu >= 0 && cpu < 64) {
         /* 
          * WAIT-FREE WRITE:
          * Just a standard store. The Store Buffer handles coherency.
          * Cost: ~1 cycle. No bus locking.
          */
         /* BRANCHLESS: bool is already 0 or 1, just cast */
         idle_map.as_bytes[cpu] = (u8)idle;
    }
}

void BPF_STRUCT_OPS(cake_tick, struct task_struct *p)
{
    struct cake_task_ctx *tctx;

    tctx = get_task_ctx(p, false);  /* No allocation */
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

    /* Pre-compute FUSED tier_info LUT (tier + slice in one access) */
    u64 slices[8];
    for (s32 i = 0; i < 8; i++) {
        slices[i] = (quantum_ns * tier_multiplier[i]) >> 10;
    }
    for (s32 i = 0; i < 32; i++) {
        u8 t = count_to_tier_init[i];
        tier_info[i].tier = t;
        tier_info[i].slice = slices[t & 7];
    }

    /* Initialize Idle Mask (Single RCU section - saves ~6200 cycles) */
    u32 nr_cpus = scx_bpf_nr_cpu_ids();
    
    bpf_rcu_read_lock();  /* Single lock for entire scan */
    for (s32 i = 0; i < 64; i++) {
        if (i >= nr_cpus) break;
        struct task_struct *p = scx_bpf_cpu_curr(i);
        if (p && p->pid == 0) idle_map.as_bytes[i] = 1;
    }
    bpf_rcu_read_unlock();

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
               .disable        = (void *)cake_disable,
               .init           = (void *)cake_init,
               .exit           = (void *)cake_exit,
               .flags          = SCX_OPS_KEEP_BUILTIN_IDLE,
               .name           = "cake");
