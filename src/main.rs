// SPDX-License-Identifier: GPL-2.0
//
// scx_cake - A sched_ext scheduler applying CAKE bufferbloat concepts
//
// This is the userspace component that loads the BPF scheduler,
// configures it, and displays statistics.

mod stats;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use clap::Parser;
use log::{info, warn};

// Include the generated interface bindings
#[allow(non_camel_case_types, non_upper_case_globals, dead_code)]
mod bpf_intf {
    include!(concat!(env!("OUT_DIR"), "/bpf_intf.rs"));
}

// Include the generated BPF skeleton
#[allow(non_camel_case_types, non_upper_case_globals, dead_code)]
mod bpf_skel {
    include!(concat!(env!("OUT_DIR"), "/bpf_skel.rs"));
}
use bpf_skel::*;

/// scx_cake: A sched_ext scheduler applying CAKE bufferbloat concepts
///
/// This scheduler adapts CAKE's DRR++ (Deficit Round Robin++) algorithm
/// for CPU scheduling, providing low-latency scheduling for gaming and
/// interactive workloads while maintaining fairness.
#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    /// Base scheduling quantum in microseconds
    #[arg(long, default_value_t = 2000)]
    quantum: u64,

    /// Extra time bonus for new flows in microseconds
    #[arg(long, default_value_t = 8000)]
    new_flow_bonus: u64,

    /// CPU usage threshold for sparse flow classification (permille, 0-1000)
    /// Lower values = more tasks classified as sparse
    #[arg(long, default_value_t = 50)]
    sparse_threshold: u64,

    /// Maximum time before forcing preemption (microseconds)
    #[arg(long, default_value_t = 100000)]
    starvation: u64,

    /// Input latency ceiling in microseconds - safety net preempts if input task waits longer
    #[arg(long, default_value_t = 1000)]
    input_latency: u64,

    /// Enable verbose debug output
    #[arg(long, short)]
    verbose: bool,

    /// Statistics update interval in seconds
    #[arg(long, default_value_t = 1)]
    interval: u64,

    /// Enable debug output in BPF
    #[arg(long)]
    debug: bool,
}

struct Scheduler<'a> {
    skel: BpfSkel<'a>,
    args: Args,
}

impl<'a> Scheduler<'a> {
    fn new(args: Args, open_object: &'a mut std::mem::MaybeUninit<libbpf_rs::OpenObject>) -> Result<Self> {
        use libbpf_rs::skel::{SkelBuilder, OpenSkel};
        
        // Open and load the BPF skeleton
        let skel_builder = BpfSkelBuilder::default();

        let mut open_skel = skel_builder
            .open(open_object)
            .context("Failed to open BPF skeleton")?;

        // Configure the scheduler via rodata (read-only data)
        if let Some(rodata) = &mut open_skel.maps.rodata_data {
            rodata.quantum_ns = args.quantum * 1000;
            rodata.new_flow_bonus_ns = args.new_flow_bonus * 1000;
            rodata.sparse_threshold = args.sparse_threshold;
            rodata.starvation_ns = args.starvation * 1000;
            rodata.input_latency_ns = args.input_latency * 1000;
            rodata.debug = args.debug;
        }

        // Load the BPF program
        let skel = open_skel
            .load()
            .context("Failed to load BPF program")?;

        Ok(Self { skel, args })
    }

    fn run(&mut self, shutdown: Arc<AtomicBool>) -> Result<()> {
        // Attach the scheduler
        let _link = self.skel
            .maps
            .cake_ops
            .attach_struct_ops()
            .context("Failed to attach scheduler")?;

        info!("scx_cake scheduler started");
        info!("  Quantum:          {} µs", self.args.quantum);
        info!("  New flow bonus:   {} µs", self.args.new_flow_bonus);
        info!("  Sparse threshold: {}‰", self.args.sparse_threshold);
        info!("  Starvation limit: {} µs", self.args.starvation);
        info!("  Input latency:    {} µs", self.args.input_latency);

        // Main loop - print statistics
        while !shutdown.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_secs(self.args.interval));

            if self.args.verbose {
                self.print_stats();
            }

            // Check for scheduler exit using the UEI
            if scx_utils::uei_exited!(&self.skel, uei) {
                match scx_utils::uei_report!(&self.skel, uei) {
                    Ok(reason) => {
                        warn!("BPF scheduler exited: {:?}", reason);
                    }
                    Err(e) => {
                        warn!("BPF scheduler exited (failed to get reason: {})", e);
                    }
                }
                break;
            }
        }

        info!("scx_cake scheduler shutting down");
        Ok(())
    }

    fn print_stats(&self) {
        let bss = match &self.skel.maps.bss_data {
            Some(bss) => bss,
            None => return,
        };
        let stats = &bss.stats;

        let total_dispatches = stats.nr_new_flow_dispatches + stats.nr_old_flow_dispatches;
        let new_pct = if total_dispatches > 0 {
            (stats.nr_new_flow_dispatches as f64 / total_dispatches as f64) * 100.0
        } else {
            0.0
        };

        let avg_wait_us = if stats.nr_waits > 0 {
            (stats.total_wait_ns / stats.nr_waits) / 1000
        } else {
            0
        };

        println!("\n=== scx_cake Statistics ===");
        println!("Dispatches: {} total ({:.1}% new-flow)", total_dispatches, new_pct);
        for (i, name) in stats::TIER_NAMES.iter().enumerate() {
            println!("  {:12} {}", format!("{}:", name), stats.nr_tier_dispatches[i]);
        }
        println!("Sparse flow: +{} promotions, -{} demotions",
                 stats.nr_sparse_promotions, stats.nr_sparse_demotions);
        println!("Input: {} events tracked, {} preempts fired",
                 stats.nr_input_events, stats.nr_input_preempts);
        println!("Wait time: avg {} µs, max {} µs",
                 avg_wait_us, stats.max_wait_ns / 1000);
    }
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or("info")
    ).init();

    let args = Args::parse();

    // Set up signal handler
    let shutdown = Arc::new(AtomicBool::new(false));
    let shutdown_clone = shutdown.clone();
    
    ctrlc::set_handler(move || {
        info!("Received shutdown signal");
        shutdown_clone.store(true, Ordering::Relaxed);
    })?;

    // Create open object for BPF - needs to outlive scheduler
    let mut open_object = std::mem::MaybeUninit::uninit();

    // Create and run the scheduler
    let mut scheduler = Scheduler::new(args, &mut open_object)?;
    scheduler.run(shutdown)?;

    Ok(())
}
