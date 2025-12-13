// SPDX-License-Identifier: GPL-2.0
//
// Statistics module for scx_cake
//
// Provides utilities for reading and formatting scheduler statistics
// from BPF maps.

/// Priority tier names (6-tier system with quantum multipliers)
pub const TIER_NAMES: [&str; 6] = ["Realtime", "Critical", "Gaming", "Interactive", "Batch", "Background"];
