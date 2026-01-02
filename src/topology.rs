use std::path::Path;
use std::fs;
use anyhow::Result;
use log::info;

pub const MAX_CPUS: usize = 64;

#[derive(Debug, Clone)]
pub struct CpuInfo {
    pub id: usize,
    pub physical_package_id: usize, // Socket
    pub l3_cache_id: usize,         // CCD/CCX (Shared L3)
    pub _core_id: usize,            // Physical Core (unused)
    pub smt_siblings: Vec<usize>,
    pub max_freq_khz: u32,
    pub cppc_perf: u32,             // 0 if unsupported
    pub is_p_core: bool,
}

pub struct Topology {
    pub cpus: Vec<CpuInfo>,
}

impl Topology {
    pub fn new() -> Result<Self> {
        let mut cpus = Vec::new();
        let cpu_root = Path::new("/sys/devices/system/cpu");

        // Simple loop 0..MAX_CPUS, assuming contiguous IDs usually
        // A robust implementation would scan all "cpu[0-9]*" dirents.
        // For scx_cake we stick to 0..64 limit anyway.
        for i in 0..MAX_CPUS {
            let cpu_dir = cpu_root.join(format!("cpu{}", i));
            if !cpu_dir.exists() {
                continue;
            }

            let phys_pkg = read_int(&cpu_dir.join("topology/physical_package_id")).unwrap_or(0);
            let core_id = read_int(&cpu_dir.join("topology/core_id")).unwrap_or(0);
            let max_freq = read_int(&cpu_dir.join("cpufreq/scaling_max_freq")).unwrap_or(0) as u32;
            let cppc_perf = read_int(&cpu_dir.join("acpi_cppc/highest_perf")).unwrap_or(0) as u32;
            
            // Detect L3 Cache ID
            // Scan cpuN/cache/index* for level=3
            let mut l3_cache_id = phys_pkg; // Fallback to socket ID if no L3 found
            for idx in 0..4 {
                let cache_dir = cpu_dir.join(format!("cache/index{}", idx));
                if !cache_dir.exists() { continue; }
                
                if let Some(level) = read_int(&cache_dir.join("level")) {
                    if level == 3 {
                        if let Some(id) = read_int(&cache_dir.join("id")) {
                            l3_cache_id = id;
                            break;
                        }
                    }
                }
            }

            // SMT siblings detection
            let mut smt_siblings = Vec::new();
            if let Ok(list) = fs::read_to_string(cpu_dir.join("topology/thread_siblings_list")) {
                smt_siblings = parse_cpu_list(&list, i);
            }

            cpus.push(CpuInfo {
                id: i,
                physical_package_id: phys_pkg as usize,
                l3_cache_id: l3_cache_id as usize,
                _core_id: core_id as usize,
                smt_siblings,
                max_freq_khz: max_freq,
                cppc_perf,
                is_p_core: true, // Will refine later
            });
        }

        // Detect P-Cores vs E-Cores based on freq outliers
        if !cpus.is_empty() {
            let avg_freq: u32 = cpus.iter().map(|c| c.max_freq_khz).sum::<u32>() / cpus.len() as u32;
            // Heuristic: If max_freq < 80% of average, it's an E-Core
            // On homogeneous systems (AMD X3D), all stay True.
            for cpu in &mut cpus {
                if cpu.max_freq_khz > 0 && cpu.max_freq_khz < (avg_freq * 8 / 10) {
                    cpu.is_p_core = false;
                }
            }
        }

        // Check for multiple L3s to log helpful info
        let mut l3_ids: Vec<usize> = cpus.iter().map(|c| c.l3_cache_id).collect();
        l3_ids.sort();
        l3_ids.dedup();
        
        info!("Detected {} CPUs. Dual-CCD/Hybrid awareness enabled. Found {} L3 Cache Domains.", cpus.len(), l3_ids.len());
        Ok(Self { cpus })
    }

    pub fn generate_scan_order(&self) -> [[u8; MAX_CPUS]; MAX_CPUS] {
        let mut table = [[255u8; MAX_CPUS]; MAX_CPUS];

        for (_i, cpu) in self.cpus.iter().enumerate() {
            // Priority List for CPU[i]:
            // Key insight: For partial load, we want PHYSICAL CORES before HT siblings
            // to avoid sharing execution resources with busy cores.
            //
            // NEW Priority Order:
            // 1. Same L3, Non-SMT (same CCD, different physical core) - best cache + no contention
            // 2. Remote L3, Non-SMT, High CPPC (different CCD but full core) - for fast cores
            // 3. Same L3, SMT sibling (only if physical cores exhausted)
            // 4. Remote L3, SMT siblings (last resort)
            //
            // This ensures we spread across physical cores first, then use HT.
            
            let mut neighbors: Vec<&CpuInfo> = self.cpus.iter()
                .filter(|c| c.id != cpu.id) // Don't include self
                .collect();

            neighbors.sort_by(|a, b| {
                // Priority 1: AVOID SMT Siblings (Physical Cores First!)
                // This is now TOP priority to prevent HT sibling preference.
                // An SMT sibling shares execution resources - bad for partial load.
                let a_is_smt = cpu.smt_siblings.contains(&a.id);
                let b_is_smt = cpu.smt_siblings.contains(&b.id);
                if a_is_smt != b_is_smt {
                    return a_is_smt.cmp(&b_is_smt); // False (Not SMT) first!
                }

                // Priority 2: Same L3 Cache? (Lowest Latency Inter-core)
                // Among non-SMT cores, prefer same CCD for cache locality.
                let a_same_l3 = a.l3_cache_id == cpu.l3_cache_id;
                let b_same_l3 = b.l3_cache_id == cpu.l3_cache_id;
                if a_same_l3 != b_same_l3 {
                    return b_same_l3.cmp(&a_same_l3); // True first
                }

                // Priority 3: CPPC Perf (Golden Core / Fast Core)
                // Higher CPPC = better single-thread performance.
                // This helps use fastest cores when lightly loaded.
                if a.cppc_perf != b.cppc_perf {
                    return b.cppc_perf.cmp(&a.cppc_perf); // Higher first
                }

                // Priority 4: P-Core?
                if a.is_p_core != b.is_p_core {
                    return b.is_p_core.cmp(&a.is_p_core); // P-Core first
                }

                // Fallback: stable sort by ID
                a.id.cmp(&b.id)
            });

            // Fill table row
            for (idx, target) in neighbors.iter().enumerate() {
                if idx >= MAX_CPUS { break; }
                table[cpu.id][idx] = target.id as u8;
            }
        }

        table
    }

    /// Generate SMT sibling map for each CPU
    /// Returns array where index is CPU ID and value is its SMT sibling (255 = none)
    pub fn generate_smt_siblings(&self) -> [u8; MAX_CPUS] {
        let mut siblings = [255u8; MAX_CPUS];
        
        for cpu in &self.cpus {
            if cpu.id >= MAX_CPUS {
                continue;
            }
            // Take first SMT sibling (if any)
            if let Some(&sibling) = cpu.smt_siblings.first() {
                if sibling < MAX_CPUS {
                    siblings[cpu.id] = sibling as u8;
                }
            }
        }
        
        siblings
    }
    
    /// Generate L3 cache ID map for each CPU (for CCX affinity)
    /// Returns array where index is CPU ID and value is its L3 cache ID
    pub fn generate_l3_ids(&self) -> [u8; MAX_CPUS] {
        let mut l3_ids = [0u8; MAX_CPUS];
        
        for cpu in &self.cpus {
            if cpu.id >= MAX_CPUS {
                continue;
            }
            l3_ids[cpu.id] = cpu.l3_cache_id as u8;
        }
        
        l3_ids
    }

    pub fn check_features(&self) -> (bool, bool) {
        let mut l3_ids = Vec::new();
        for cpu in &self.cpus {
            if !l3_ids.contains(&cpu.l3_cache_id) {
                l3_ids.push(cpu.l3_cache_id);
            }
        }
        let is_dual_ccd = l3_ids.len() > 1;

        let is_hybrid = self.cpus.iter().any(|c| !c.is_p_core);

        (is_dual_ccd, is_hybrid)
    }
}

// Helpers
fn read_int(path: &Path) -> Option<usize> {
    if let Ok(s) = fs::read_to_string(path) {
        // Handle "0-63" or just "0" - we just want integer value if single
        // For cpufreq/cppc it's single int.
        if let Ok(v) = s.trim().parse::<usize>() {
            return Some(v);
        }
    }
    None
}

fn parse_cpu_list(list: &str, self_id: usize) -> Vec<usize> {
    let mut vec = Vec::new();
    // Parse "0-7" or "0,1,2" format. Rudimentary parser.
    // For SMT siblings, it's usually "0,8"
    for part in list.split(',') {
        if let Some(range_idx) = part.find('-') {
            // Range
            let start: usize = part[..range_idx].trim().parse().unwrap_or(0);
            let end: usize = part[range_idx+1..].trim().parse().unwrap_or(0);
            for i in start..=end {
                if i != self_id { vec.push(i); }
            }
        } else {
            // Single
            if let Ok(v) = part.trim().parse::<usize>() {
                if v != self_id { vec.push(v); }
            }
        }
    }
    vec
}
