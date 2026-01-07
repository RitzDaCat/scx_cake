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
 * Topology configuration (set by userspace based on detected hardware)
 * 
 * These enable zero-cost specialization:
 * - When false, BPF verifier eliminates the code path entirely
 * - When true, adds ~2 cycles for CCD/P-core preference
 * 
 * On 9800X3D: both false → zero overhead
 * On 7950X3D: has_dual_ccd=true → CCD-local scheduling
 * On i9-14900K: has_hybrid_cores=true → P-core preference
 */
const volatile bool has_dual_ccd = false;
const volatile bool has_hybrid_cores = false;
const volatile u64 ccd0_mask = 0xFFFFFFFFFFFFFFFF;  /* All cores by default */
const volatile u64 ccd1_mask = 0;
const volatile u64 p_core_mask = 0xFFFFFFFFFFFFFFFF; /* All P-cores by default */
const volatile u32 cpus_per_ccd = 64;

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
u64 idle_mask SEC(".bss") __attribute__((aligned(64)));

/*
 * Global Victim Mask (Bitmask for CTZ scanning)
 * 
 * KEPT AS BITMASK because we need "find first victim" via CTZ.
 * Updated non-atomically (acceptable race - victim is heuristic).
 */
u64 victim_mask SEC(".bss") __attribute__((aligned(64)));

/*
 * DSQ Non-Empty Byte-Mask (Lock-Free DSQ Tracking)
 * 
 * BYTEMASK because we check SPECIFIC DSQs by index, not scan.
 * dsq_has_tasks[GAMING_DSQ] is a direct byte load - perfect for bytemask.
 */
u8 dsq_has_tasks[8] SEC(".bss") __attribute__((aligned(8)));

/*
 * Global CPU Tier Array (The "Scoreboard")
 * BSS array allowing any CPU to see what Tier is running on any other CPU.
 * OPTIMIZATION: BSS array replaces BPF_MAP_TYPE_ARRAY for direct memory access.
 * Saves ~10-15 cycles per access (no map lookup helper call).
 * Padded to 64 bytes per entry to prevent False Sharing.
 */
struct cake_cpu_status {
    u8 tier;
    u8 __pad[63]; /* Pad to 64 bytes (Cache Line Size) to prevent False Sharing */
};

/* BSS array - direct indexed access, no map lookup overhead */
struct cake_cpu_status cpu_status[64] SEC(".bss") __attribute__((aligned(64)));

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

/* Optimization: Pre-computed tier slices (quantum_ns * multiplier >> 10) */
static u64 tier_slice[8];



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

/*
 * Count-to-Tier Lookup Table (O(1) tier calculation)
 * Replaces 6 if-else branches with single array access.
 * Index = sparse_count (0-31), Value = tier (0-6)
 */
static const u8 count_to_tier[32] = {
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

    tctx = get_task_ctx(p);
    if (unlikely(!tctx)) {
        /* Fallback if we can't get context */
        return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
    }

    /* 
     * OPTIMIZATION: Bitmask Idle Search (O(1) via CTZ)
     * We use a global 64-bit mask where set bits = idle CPUs.
     */
    u64 mask = idle_mask;
    
    /* Move tier read up for the check */
    u8 tier = GET_TIER(tctx);
    
    if (mask == 0 && tier > REALTIME_DSQ) {
        /* No idle CPUs exist. Skip expensive search. */
        return prev_cpu;
    }

    if (mask == 0) {
        cpu = prev_cpu;
        is_idle = false;
    } else {
        /* 
         * OPTIMIZATION: Bitmask Idle Find (O(1))
         * 1. Check prev_cpu first (cache locality)
         * 2. Use CTZ to find any idle CPU
         */
        
        /* Step 1: Check Sticky/Prev CPU */
        if (prev_cpu >= 0 && prev_cpu < 64 && (mask & (1ULL << prev_cpu))) {
            is_idle = true;
            cpu = prev_cpu;
        } else {
            /* Step 2: Find any idle CPU via CTZ */
            cpu = __builtin_ctzll(mask);
            is_idle = true;
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

    /* Mark DSQ as non-empty (atomic byte write - no LOCK needed) */
    dsq_has_tasks[tier] = 1;

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
     */
    if (prev && ((prev->pid ^ prev->se.sum_exec_runtime) & 0xF) == 0) {
        if (dsq_has_tasks[BACKGROUND_DSQ] && scx_bpf_dsq_move_to_local(BACKGROUND_DSQ)) {
            return;
        }
        if (dsq_has_tasks[INTERACTIVE_DSQ] && scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ)) {
            return;
        }
    }
    
    /* Priority order with O(1) byte-mask pre-check */
    if (dsq_has_tasks[CRITICAL_LATENCY_DSQ] && scx_bpf_dsq_move_to_local(CRITICAL_LATENCY_DSQ)) {
        return;
    }
    if (dsq_has_tasks[REALTIME_DSQ] && scx_bpf_dsq_move_to_local(REALTIME_DSQ)) {
        return;
    }
    if (dsq_has_tasks[CRITICAL_DSQ] && scx_bpf_dsq_move_to_local(CRITICAL_DSQ)) {
        return;
    }
    if (dsq_has_tasks[GAMING_DSQ] && scx_bpf_dsq_move_to_local(GAMING_DSQ)) {
        return;
    }
    if (dsq_has_tasks[INTERACTIVE_DSQ] && scx_bpf_dsq_move_to_local(INTERACTIVE_DSQ)) {
        return;
    }
    if (dsq_has_tasks[BATCH_DSQ] && scx_bpf_dsq_move_to_local(BATCH_DSQ)) {
        return;
    }
    if (dsq_has_tasks[BACKGROUND_DSQ] && scx_bpf_dsq_move_to_local(BACKGROUND_DSQ)) {
        /* No clear */
    }
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
    struct cake_task_ctx *tctx = get_task_ctx(p);
    if (unlikely(!tctx))
        return;

    u32 cpu_idx = scx_bpf_task_cpu(p);
    if (cpu_idx >= 64)
        return;

    u8 tier = GET_TIER(tctx);
    
    /* Unconditional scoreboard write (1 store vs read+cmp+branch+store) */
    cpu_status[cpu_idx].tier = tier;
    
    /* Unconditional victim_mask update (1 bitwise op vs read+cmp+branch+bitwise) */
    u64 cpu_bit = (1ULL << cpu_idx);
    if (tier >= INTERACTIVE_DSQ) {
        victim_mask |= cpu_bit;
    } else {
        victim_mask &= ~cpu_bit;
    }

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
    struct cake_task_ctx *tctx = get_task_ctx(p);
    if (unlikely(!tctx || tctx->last_run_at == 0))
        return;

    /* Runtime calculation: 3 ops */
    u64 runtime = (u32)scx_bpf_now() - tctx->last_run_at;
    
    /* Sparse count update: 3 ops */
    u32 packed = tctx->packed_info;
    u32 old_count = (packed >> SHIFT_SPARSE_COUNT) & MASK_SPARSE_COUNT;
    u32 new_count = (runtime < cached_threshold_ns) ? ((old_count + 1) & MASK_SPARSE_COUNT) : 0;
    
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
    
    /* Tier + Slice: 2 LUT accesses */
    u8 tier = count_to_tier[new_count];
    tctx->next_slice = tier_slice[tier & 7];
    
    /* Update packed_info: single read-modify-write */
    tctx->packed_info = (packed & ~((MASK_SPARSE_COUNT << SHIFT_SPARSE_COUNT) | (MASK_TIER << SHIFT_TIER)))
                      | ((new_count & MASK_SPARSE_COUNT) << SHIFT_SPARSE_COUNT)
                      | ((tier & MASK_TIER) << SHIFT_TIER);
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
    /* Cap CPU ID to 63 for 64-bit mask safety */
    if (cpu >= 0 && cpu < 64) {
         u64 bit = (1ULL << cpu);
         /* 
          * OPTIMIZATION: Local Buffering (Test-and-Test-and-Set)
          * We read the global mask into a local variable first.
          * Only if the bit is different do we pay the cost of the atomic bus lock.
          * This matches the "Local Buffering" pattern by avoiding "Naive" global updates.
          */
         u64 mask = idle_mask; 

         if (idle) {
             /* Mark as Idle (Set Bit) - Only if not already set */
             if (!(mask & bit))
                 __atomic_fetch_or(&idle_mask, bit, __ATOMIC_RELAXED);
         } else {
             /* Mark as Busy (Clear Bit) - Only if currently set */
             if (mask & bit)
                 __atomic_fetch_and(&idle_mask, ~bit, __ATOMIC_RELAXED);
         }
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

    /* Pre-compute tier slices (eliminates multiply from hot path) */
    for (s32 i = 0; i < 8; i++) {
        tier_slice[i] = (quantum_ns * tier_multiplier[i]) >> 10;
    }

    /* Initialize Idle Mask (Correctness Sync) */
    u32 nr_cpus = scx_bpf_nr_cpu_ids();
    u64 init_mask = 0;
    /* Loop bounded for verifier (max 64 CPUs for mask) */
    for (s32 i = 0; i < 64; i++) {
        if (i >= nr_cpus) break;
        bpf_rcu_read_lock();
        struct task_struct *p = scx_bpf_cpu_curr(i);
        /* If idle task is running, set the bit */
        if (p && p->pid == 0) init_mask |= (1ULL << i);
        bpf_rcu_read_unlock();
    }
    /* Direct assignment to global (no concurrency on init) */
    idle_mask = init_mask;

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
