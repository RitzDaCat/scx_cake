#include "vmlinux.h"
#include "scx/common.bpf.h"

char _license[] SEC("license") = "GPL";

/*
 * Data Structures:
 * Global array struct task_struct *slots[256]
 * Bitmask u64 occupancy_mask[4]
 */
struct task_struct *slots[256] SEC(".bss");
u64 occupancy_mask[4] SEC(".bss");

/*
 * Entropy Logic:
 * Use scx_bpf_now() and bpf_get_smp_processor_id() to generate a 1-cycle index.
 */
static __always_inline u32 get_slot_index(void)
{
	return (scx_bpf_now() ^ bpf_get_smp_processor_id()) & 0xFF;
}

static __always_inline void mark_slot_occupied(u32 idx)
{
	u32 word = idx / 64;
	u64 bit = 1ULL << (idx % 64);
	__sync_fetch_and_or(&occupancy_mask[word], bit);
}

static __always_inline void mark_slot_empty(u32 idx)
{
	u32 word = idx / 64;
	u64 bit = 1ULL << (idx % 64);
	__sync_fetch_and_and(&occupancy_mask[word], ~bit);
}

/*
 * Scrubber Logic:
 * Prevents tasks from being stranded in slots if the system enters a low-load state.
 * Uses xchg to extract tasks and dispatches them to the global DSQ.
 */
static void scrub_slots(void)
{
	struct task_struct *p;
	int i;

	/*
	 * Brute-force scan of all slots.
	 * Since this is a background maintenance task, we prioritize correctness
	 * over extreme optimization, ensuring no task is left behind.
	 */
	bpf_for(i, 0, 256) {
		p = (struct task_struct *)__sync_lock_test_and_set((unsigned long *)&slots[i], 0);
		if (p) {
			mark_slot_empty(i);
			scx_bpf_dsq_insert(p, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, 0);
		}
	}
}

void BPF_STRUCT_OPS(stochastic_enqueue, struct task_struct *p, u64 enq_flags)
{
	u32 idx = get_slot_index();
	struct task_struct *old;

	/*
	 * Wait-Free Dispatch:
	 * Use xchg to drop tasks into slots.
	 */
	old = (struct task_struct *)__sync_lock_test_and_set((unsigned long *)&slots[idx], (unsigned long)p);

	if (old) {
		/*
		 * Collision (slot already full).
		 * Fallback to SCX_DSQ_GLOBAL for the displaced task.
		 */
		scx_bpf_dsq_insert(old, SCX_DSQ_GLOBAL, SCX_SLICE_DFL, 0);
	} else {
		/*
		 * Success (slot was empty).
		 * Update the mask AFTER the xchg (as requested).
		 */
		mark_slot_occupied(idx);
	}
}

/*
 * Helper macro to process a mask word in the dispatch loop.
 * Unrolled to avoid loop overhead.
 */
#define TRY_DISPATCH_FROM_WORD(word_idx) do {                               \
	u64 mask = occupancy_mask[word_idx];                                \
	if (mask) {                                                         \
		/* Fast-Scan: __builtin_ctzll to find task in 1 cycle */    \
		u64 bit_idx = __builtin_ctzll(mask);                        \
		u32 idx = word_idx * 64 + bit_idx;                          \
                                                                            \
		/* Atomically clear the bit */                              \
		u64 bit = 1ULL << bit_idx;                                  \
		__sync_fetch_and_and(&occupancy_mask[word_idx], ~bit);      \
                                                                            \
		/* Opportunistically claim the task */                      \
		struct task_struct *p = (struct task_struct *)__sync_lock_test_and_set((unsigned long *)&slots[idx], 0); \
                                                                            \
		/* Verify if we actually got a task */                      \
		if (p) {                                                    \
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0); \
			return; /* Consumed one task, return immediately */ \
		}                                                           \
		/* If p was NULL, it was a race (transient state), continue */ \
	}                                                                   \
} while(0)

void BPF_STRUCT_OPS(stochastic_dispatch, s32 cpu, struct task_struct *prev)
{
	/*
	 * Fast-Scan Consumer:
	 * Manually unrolled scan of the 4 u64 words of the occupancy_mask.
	 * This ensures minimal overhead and hits the 1-cycle arithmetic goal
	 * for finding a candidate.
	 */
	TRY_DISPATCH_FROM_WORD(0);
	TRY_DISPATCH_FROM_WORD(1);
	TRY_DISPATCH_FROM_WORD(2);
	TRY_DISPATCH_FROM_WORD(3);
}

void BPF_STRUCT_OPS(stochastic_cpu_release, s32 cpu, struct scx_cpu_release_args *args)
{
	/* Safety: Scrub slots when CPU is released (potential low load) */
	scrub_slots();
}

void BPF_STRUCT_OPS(stochastic_tick, struct task_struct *p)
{
	/* Safety: Periodic scrub */
	scrub_slots();
}

/*
 * Define the sched_ext ops
 */
SEC(".struct_ops.link")
struct sched_ext_ops stochastic_ops = {
	.enqueue	= (void *)stochastic_enqueue,
	.dispatch	= (void *)stochastic_dispatch,
	.cpu_release	= (void *)stochastic_cpu_release,
	.tick		= (void *)stochastic_tick,
	.name		= "stochastic",
};
