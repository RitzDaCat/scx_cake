use std::path::Path;
use std::fs;
use anyhow::Result;
use log::info;

pub const MAX_CPUS: usize = 64;

#[derive(Debug, Clone)]
pub struct CpuInfo {
    pub id: usize,
    pub physical_package_id: usize, // CCD/Socket
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

            // SMT siblings detection
            let mut smt_siblings = Vec::new();
            if let Ok(list) = fs::read_to_string(cpu_dir.join("topology/thread_siblings_list")) {
                smt_siblings = parse_cpu_list(&list, i);
            }

            cpus.push(CpuInfo {
                id: i,
                physical_package_id: phys_pkg as usize,
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

        info!("Detected {} CPUs. Dual-CCD/Hybrid awareness enabled.", cpus.len());
        Ok(Self { cpus })
    }

    pub fn generate_scan_order(&self) -> [[u8; MAX_CPUS]; MAX_CPUS] {
        let mut table = [[255u8; MAX_CPUS]; MAX_CPUS];

        for (_i, cpu) in self.cpus.iter().enumerate() {
            // Priority List for CPU[i]:
            // 1. SMT Siblings (Fastest Context Switch/Cache sharing)
            // 2. Same CCD, P-Cores, High CPPC
            // 3. Same CCD, P-Cores, Low CPPC
            // 4. Same CCD, E-Cores
            // 5. Remote CCD, P-Cores...
            // 6. ...
            
            let mut neighbors: Vec<&CpuInfo> = self.cpus.iter()
                .filter(|c| c.id != cpu.id) // Don't include self
                .collect();

            neighbors.sort_by(|a, b| {
                // Priority 1: Same CCD? (L3 Cache)
                let a_same_ccd = a.physical_package_id == cpu.physical_package_id;
                let b_same_ccd = b.physical_package_id == cpu.physical_package_id;
                if a_same_ccd != b_same_ccd {
                    return b_same_ccd.cmp(&a_same_ccd); // True first
                }

                // Priority 2: P-Core?
                if a.is_p_core != b.is_p_core {
                    return b.is_p_core.cmp(&a.is_p_core); // P-Core first
                }

                // Priority 3: AVOID SMT Siblings (Physical Cores First)
                // We want to fill physical cores before using hyperthreads.
                let a_is_smt = cpu.smt_siblings.contains(&a.id);
                let b_is_smt = cpu.smt_siblings.contains(&b.id);
                if a_is_smt != b_is_smt {
                    return a_is_smt.cmp(&b_is_smt); // False (Not SMT) first!
                }

                // Priority 4: CPPC Perf (Golden Core)
                // Use descending order (Higher is better)
                if a.cppc_perf != b.cppc_perf {
                    return b.cppc_perf.cmp(&a.cppc_perf);
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

    pub fn check_features(&self) -> (bool, bool) {
        let mut package_ids = Vec::new();
        for cpu in &self.cpus {
            if !package_ids.contains(&cpu.physical_package_id) {
                package_ids.push(cpu.physical_package_id);
            }
        }
        let is_dual_ccd = package_ids.len() > 1;

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
