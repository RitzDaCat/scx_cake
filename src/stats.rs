// SPDX-License-Identifier: GPL-2.0
//
// Statistics module for scx_cake
//
// Provides utilities for reading and formatting scheduler statistics
// from BPF maps.

/// Priority tier names (7-tier system with quantum multipliers)
pub const TIER_NAMES: [&str; 7] = ["DSQ0", "DSQ1", "DSQ2", "DSQ3", "DSQ4", "DSQ5", "DSQ6"];

/// Number of shards per tier
pub const SHARD_COUNT: usize = 2;
