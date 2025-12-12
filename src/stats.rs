// SPDX-License-Identifier: GPL-2.0
//
// Statistics module for scx_cake
//
// Provides utilities for reading and formatting scheduler statistics
// from BPF maps.

/// Priority tier names (matching CAKE's DiffServ tins)
pub const TIER_NAMES: [&str; 4] = ["Critical", "Gaming", "Interactive", "Background"];
