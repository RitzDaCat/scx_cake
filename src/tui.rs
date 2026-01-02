// SPDX-License-Identifier: GPL-2.0
//
// TUI module for scx_cake
//
// Provides a ratatui-based terminal UI for real-time scheduler statistics.

use std::io::{self, Stdout};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use arboard::Clipboard;
use crossterm::{
    event::{self, Event, KeyCode, KeyEventKind},
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
    ExecutableCommand,
};
use ratatui::{
    prelude::*,
    widgets::{Block, Borders, Cell, Paragraph, Row, Table},
};

use crate::bpf_skel::types::cake_stats;
use crate::bpf_skel::BpfSkel;
use crate::stats::{TIER_NAMES, SHARD_COUNT};
use crate::topology::TopologySummary;
use libbpf_rs::{MapFlags, MapCore};

fn aggregate_stats(map: &libbpf_rs::Map) -> Result<cake_stats> {
    let key = 0u32;
    let key_bytes = key.to_ne_bytes();
    
    // Per-CPU map lookup returns values for all CPUs
    // We treat key 0 as the single bucket containing stats for all CPUs
    let values = match map.lookup_percpu(&key_bytes, MapFlags::ANY) {
        Ok(Some(v)) => v,
        _ => return Ok(Default::default()), // Handle error or missing key
    };

    let mut total: cake_stats = Default::default();

    // Iterate over each CPU's stats and sum them up
    if let Some(first_cpu_bytes) = values.first() {
        // Verify size
        if first_cpu_bytes.len() != std::mem::size_of::<cake_stats>() {
            return Ok(Default::default()); // Safety check
        }
    }

    for cpu_bytes in values {
        if cpu_bytes.len() != std::mem::size_of::<cake_stats>() {
            continue;
        }
        
        // Deserialize bytes to struct (unsafe fetch)
        let s: cake_stats = unsafe { std::ptr::read_unaligned(cpu_bytes.as_ptr() as *const _) };

        // Sum all fields
        total.nr_new_flow_dispatches += s.nr_new_flow_dispatches;
        total.nr_old_flow_dispatches += s.nr_old_flow_dispatches;
        
        for i in 0..TIER_NAMES.len() {
            total.nr_tier_dispatches[i] += s.nr_tier_dispatches[i];
            total.nr_wait_demotions_tier[i] += s.nr_wait_demotions_tier[i];
            total.nr_starvation_preempts_tier[i] += s.nr_starvation_preempts_tier[i];
            total.total_wait_ns_tier[i] += s.total_wait_ns_tier[i];
            total.nr_waits_tier[i] += s.nr_waits_tier[i];
            // Max is max, not sum
            if s.max_wait_ns_tier[i] > total.max_wait_ns_tier[i] {
                total.max_wait_ns_tier[i] = s.max_wait_ns_tier[i];
            }
            // Sum per-shard dispatches and latency
            for sh in 0..SHARD_COUNT {
                total.nr_shard_dispatches[i][sh] += s.nr_shard_dispatches[i][sh];
                total.total_wait_ns_shard[i][sh] += s.total_wait_ns_shard[i][sh];
                total.nr_waits_shard[i][sh] += s.nr_waits_shard[i][sh];
                if s.max_wait_ns_shard[i][sh] > total.max_wait_ns_shard[i][sh] {
                    total.max_wait_ns_shard[i][sh] = s.max_wait_ns_shard[i][sh];
                }
            }
        }
        
        total.nr_sparse_promotions += s.nr_sparse_promotions;
        total.nr_sparse_demotions += s.nr_sparse_demotions;
        total.nr_wait_demotions += s.nr_wait_demotions;
        total.total_wait_ns += s.total_wait_ns;
        total.nr_waits += s.nr_waits;
        if s.max_wait_ns > total.max_wait_ns {
            total.max_wait_ns = s.max_wait_ns;
        }
        total.nr_input_preempts += s.nr_input_preempts;
    }

    Ok(total)
}

/// TUI Application state
pub struct TuiApp {
    start_time: Instant,
    status_message: Option<(String, Instant)>,
}

impl TuiApp {
    pub fn new() -> Self {
        Self {
            start_time: Instant::now(),
            status_message: None,
        }
    }

    /// Format uptime as "Xm Ys" or "Xh Ym"
    fn format_uptime(&self) -> String {
        let elapsed = self.start_time.elapsed();
        let secs = elapsed.as_secs();
        if secs < 3600 {
            format!("{}m {}s", secs / 60, secs % 60)
        } else {
            format!("{}h {}m", secs / 3600, (secs % 3600) / 60)
        }
    }

    /// Set a temporary status message that disappears after 2 seconds
    fn set_status(&mut self, msg: &str) {
        self.status_message = Some((msg.to_string(), Instant::now()));
    }

    /// Get current status message if not expired
    fn get_status(&self) -> Option<&str> {
        match &self.status_message {
            Some((msg, timestamp)) if timestamp.elapsed() < Duration::from_secs(2) => Some(msg),
            _ => None,
        }
    }
}

/// Initialize the terminal for TUI mode
fn setup_terminal() -> Result<Terminal<CrosstermBackend<Stdout>>> {
    enable_raw_mode().context("Failed to enable raw mode")?;
    io::stdout()
        .execute(EnterAlternateScreen)
        .context("Failed to enter alternate screen")?;
    let backend = CrosstermBackend::new(io::stdout());
    Terminal::new(backend).context("Failed to create terminal")
}

/// Restore terminal to normal mode
fn restore_terminal() -> Result<()> {
    disable_raw_mode().context("Failed to disable raw mode")?;
    io::stdout()
        .execute(LeaveAlternateScreen)
        .context("Failed to leave alternate screen")?;
    Ok(())
}

/// Format stats as a copyable text string
fn format_stats_for_clipboard(stats: &cake_stats, uptime: &str) -> String {
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

    // Helper to format avg latency in µs per shard
    let avg_lat_us = |tier: usize, shard: usize| -> u64 {
        if stats.nr_waits_shard[tier][shard] > 0 {
            (stats.total_wait_ns_shard[tier][shard] / stats.nr_waits_shard[tier][shard]) / 1000
        } else {
            0
        }
    };

    let mut output = String::new();
    output.push_str(&format!("=== scx_cake Statistics (Uptime: {}) ===\n\n", uptime));
    output.push_str(&format!("Dispatches: {} total ({:.1}% new-flow)\n\n", total_dispatches, new_pct));
    
    output.push_str("DSQ      Dispatches    Max Wait    WaitDemote  StarvPreempt  Shard0   Shard1   S0 Lat   S1 Lat\n");
    output.push_str("────────────────────────────────────────────────────────────────────────────────────────────────\n");
    for (i, name) in TIER_NAMES.iter().enumerate() {
        let max_wait_us = stats.max_wait_ns_tier[i] / 1000;
        let lat0 = avg_lat_us(i, 0);
        let lat1 = avg_lat_us(i, 1);
        output.push_str(&format!(
            "{:6}   {:>10}    {:>6} µs    {:>10}  {:>12}  {:>7}  {:>7}  {:>5} µs {:>5} µs\n",
            name, stats.nr_tier_dispatches[i], max_wait_us,
            stats.nr_wait_demotions_tier[i], stats.nr_starvation_preempts_tier[i],
            stats.nr_shard_dispatches[i][0], stats.nr_shard_dispatches[i][1],
            lat0, lat1
        ));
    }
    
    output.push_str(&format!("\nSparse flow: +{} promotions, -{} demotions, {} wait-demotes\n",
        stats.nr_sparse_promotions, stats.nr_sparse_demotions, stats.nr_wait_demotions));
    output.push_str(&format!("Input: {} preempts fired\n", stats.nr_input_preempts));
    output.push_str(&format!("Wait time: avg {} µs, max {} µs\n",
        avg_wait_us, stats.max_wait_ns / 1000));
    
    output
}

/// Draw the UI
fn draw_ui(frame: &mut Frame, app: &TuiApp, stats: &cake_stats, topo: &TopologySummary) {
    let area = frame.area();

    // Create main layout: header, topology info, stats table, shard usage, summary, footer
    let layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),  // Header
            Constraint::Length(3),  // Topology info
            Constraint::Length(10), // Stats table
            Constraint::Length(7),  // Shard usage with latency (4 lines + borders)
            Constraint::Length(5),  // Summary
            Constraint::Length(3),  // Footer
        ])
        .split(area);

    // --- Header ---
    let total_dispatches = stats.nr_new_flow_dispatches + stats.nr_old_flow_dispatches;
    let new_pct = if total_dispatches > 0 {
        (stats.nr_new_flow_dispatches as f64 / total_dispatches as f64) * 100.0
    } else {
        0.0
    };
    let header_text = format!(
        " Dispatches: {} total ({:.1}% new-flow)  │  Uptime: {}",
        total_dispatches, new_pct, app.format_uptime()
    );
    let header = Paragraph::new(header_text)
        .block(Block::default()
            .title(" scx_cake Statistics ")
            .title_style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Blue)));
    frame.render_widget(header, layout[0]);

    // --- Topology Info ---
    let smt_str = if topo.has_smt { "SMT" } else { "No SMT" };
    let hybrid_str = if topo.is_hybrid { ", Hybrid" } else { "" };
    let cppc_str = if topo.has_cppc { ", CPPC" } else { "" };
    let topo_text = format!(
        " {} CPUs ({} cores) │ {} socket(s) │ {} L3/CCDs │ {} MHz │ {}{}{}",
        topo.num_cpus, topo.num_cores, topo.num_sockets, topo.num_l3_domains,
        topo.max_freq_mhz, smt_str, hybrid_str, cppc_str
    );
    let topo_widget = Paragraph::new(topo_text)
        .style(Style::default().fg(Color::White))
        .block(Block::default()
            .title(" CPU Topology ")
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Magenta)));
    frame.render_widget(topo_widget, layout[1]);

    // --- Stats Table ---
    let header_cells = ["DSQ", "Dispatches", "Max Wait", "WaitDemote", "StarvPreempt"]
        .iter()
        .map(|h| Cell::from(*h).style(Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)));
    let header_row = Row::new(header_cells).height(1);

    let rows: Vec<Row> = TIER_NAMES
        .iter()
        .enumerate()
        .map(|(i, name)| {
            let max_wait_us = stats.max_wait_ns_tier[i] / 1000;
            let cells = vec![
                Cell::from(*name).style(tier_style(i)),
                Cell::from(format!("{}", stats.nr_tier_dispatches[i])),
                Cell::from(format!("{} µs", max_wait_us)),
                Cell::from(format!("{}", stats.nr_wait_demotions_tier[i])),
                Cell::from(format!("{}", stats.nr_starvation_preempts_tier[i])),
            ];
            Row::new(cells).height(1)
        })
        .collect();

    let table = Table::new(
        rows,
        [
            Constraint::Length(8),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(14),
        ],
    )
    .header(header_row)
    .block(Block::default()
        .title(" Per-DSQ Statistics ")
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Blue)));
    frame.render_widget(table, layout[2]);

    // --- Shard Usage with Latency ---
    // Build a compact view showing balance and avg latency per shard
    let mut shard_lines: Vec<String> = Vec::new();
    
    // Helper to format avg latency in µs
    let avg_lat_us = |tier: usize, shard: usize| -> u64 {
        if stats.nr_waits_shard[tier][shard] > 0 {
            (stats.total_wait_ns_shard[tier][shard] / stats.nr_waits_shard[tier][shard]) / 1000
        } else {
            0
        }
    };
    
    // First line: DSQ0-3 with balance percentages
    let mut line1_parts: Vec<String> = Vec::new();
    for i in 0..4 {
        let s0 = stats.nr_shard_dispatches[i][0];
        let s1 = stats.nr_shard_dispatches[i][1];
        let total = s0 + s1;
        let pct0 = if total > 0 { (s0 as f64 / total as f64 * 100.0) as u32 } else { 50 };
        line1_parts.push(format!("{}[{}%:{}%]", TIER_NAMES[i], pct0, 100 - pct0));
    }
    shard_lines.push(format!(" {}", line1_parts.join("  ")));
    
    // Second line: DSQ0-3 latencies (avg µs per shard)
    let mut lat1_parts: Vec<String> = Vec::new();
    for i in 0..4 {
        let lat0 = avg_lat_us(i, 0);
        let lat1 = avg_lat_us(i, 1);
        lat1_parts.push(format!("    {}µs:{}µs", lat0, lat1));
    }
    shard_lines.push(format!(" {}", lat1_parts.join(" ")));
    
    // Third line: DSQ4-6 with balance percentages
    let mut line2_parts: Vec<String> = Vec::new();
    for i in 4..7 {
        let s0 = stats.nr_shard_dispatches[i][0];
        let s1 = stats.nr_shard_dispatches[i][1];
        let total = s0 + s1;
        let pct0 = if total > 0 { (s0 as f64 / total as f64 * 100.0) as u32 } else { 50 };
        line2_parts.push(format!("{}[{}%:{}%]", TIER_NAMES[i], pct0, 100 - pct0));
    }
    shard_lines.push(format!(" {}", line2_parts.join("  ")));
    
    // Fourth line: DSQ4-6 latencies (avg µs per shard)
    let mut lat2_parts: Vec<String> = Vec::new();
    for i in 4..7 {
        let lat0 = avg_lat_us(i, 0);
        let lat1 = avg_lat_us(i, 1);
        lat2_parts.push(format!("    {}µs:{}µs", lat0, lat1));
    }
    shard_lines.push(format!(" {}", lat2_parts.join(" ")));
    
    let shard_text = shard_lines.join("\n");
    let shard_widget = Paragraph::new(shard_text)
        .block(Block::default()
            .title(" Shard Balance & Latency (S0:S1) ")
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Blue)));
    frame.render_widget(shard_widget, layout[3]);

    // --- Summary ---
    let avg_wait_us = if stats.nr_waits > 0 {
        (stats.total_wait_ns / stats.nr_waits) / 1000
    } else {
        0
    };
    let summary_text = format!(
        " Sparse flow: +{} promotions, -{} demotions, {} wait-demotes\n \
         Input: {} preempts fired\n \
         Wait time: avg {} µs, max {} µs (overall)",
        stats.nr_sparse_promotions, stats.nr_sparse_demotions, stats.nr_wait_demotions,
        stats.nr_input_preempts,
        avg_wait_us, stats.max_wait_ns / 1000
    );
    let summary = Paragraph::new(summary_text)
        .block(Block::default()
            .title(" Summary ")
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Blue)));
    frame.render_widget(summary, layout[4]);

    // --- Footer (key bindings + status) ---
    let footer_text = match app.get_status() {
        Some(status) => format!(" [q] Quit  [c] Copy  [r] Reset  │  {}", status),
        None => " [q] Quit  [c] Copy to clipboard  [r] Reset stats".to_string(),
    };
    let (fg_color, border_color) = if app.get_status().is_some() {
        (Color::Green, Color::Green)
    } else {
        (Color::DarkGray, Color::DarkGray)
    };
    let footer = Paragraph::new(footer_text)
        .style(Style::default().fg(fg_color))
        .block(Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(border_color)));
    frame.render_widget(footer, layout[5]);
}

/// Get color style for a DSQ tier
fn tier_style(tier: usize) -> Style {
    match tier {
        0 => Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD),   // DSQ0 - highest priority
        1 => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),    // DSQ1
        2 => Style::default().fg(Color::Magenta),                              // DSQ2
        3 => Style::default().fg(Color::Green),                                // DSQ3
        4 => Style::default().fg(Color::Yellow),                               // DSQ4
        5 => Style::default().fg(Color::Blue),                                 // DSQ5
        6 => Style::default().fg(Color::DarkGray),                             // DSQ6 - lowest priority
        _ => Style::default(),
    }
}

/// Run the TUI event loop
pub fn run_tui(
    skel: &mut BpfSkel,
    shutdown: Arc<AtomicBool>,
    interval_secs: u64,
    topo: &TopologySummary,
) -> Result<()> {
    let mut terminal = setup_terminal()?;
    let mut app = TuiApp::new();
    let tick_rate = Duration::from_secs(interval_secs);
    let mut last_tick = Instant::now();
    
    // Initialize clipboard (may fail on headless systems)
    let mut clipboard = Clipboard::new().ok();

    loop {
        // Check for shutdown signal
        if shutdown.load(Ordering::Relaxed) {
            break;
        }

        // Check for UEI exit
        if scx_utils::uei_exited!(skel, uei) {
            break;
        }

        // Get current stats (aggregate from per-cpu map)
        let stats = match aggregate_stats(&skel.maps.stats_map) {
            Ok(s) => s,
            Err(_) => Default::default(),
        };

        // Draw UI
        terminal.draw(|frame| draw_ui(frame, &app, &stats, topo))?;

        // Handle events with timeout
        let timeout = tick_rate.saturating_sub(last_tick.elapsed());
        if event::poll(timeout)? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    match key.code {
                        KeyCode::Char('q') | KeyCode::Esc => {
                            shutdown.store(true, Ordering::Relaxed);
                            break;
                        }
                        KeyCode::Char('c') => {
                            // Copy stats to clipboard
                            let text = format_stats_for_clipboard(&stats, &app.format_uptime());
                            match &mut clipboard {
                                Some(cb) => {
                                    match cb.set_text(text) {
                                        Ok(_) => app.set_status("✓ Copied to clipboard!"),
                                        Err(_) => app.set_status("✗ Failed to copy"),
                                    }
                                }
                                None => app.set_status("✗ Clipboard not available"),
                            }
                        }
                        KeyCode::Char('r') => {
                            // Reset stats
                            // Reset stats (clear the map)
                            let key = 0u32;
                            let key_bytes = key.to_ne_bytes();
                            // We can't strictly "reset" per-cpu easily without writing zeros to all cpus
                            // For now, simpler to just treat 'r' as soft-reset in UI, but BPF keeps counting?
                            // Or we write a zeroed struct to all CPUs.
                            let zero_struct = cake_stats::default();
                            // Serialize to bytes
                            let zero_bytes = unsafe { 
                                std::slice::from_raw_parts(
                                    &zero_struct as *const _ as *const u8,
                                    std::mem::size_of::<cake_stats>()
                                )
                            };

                            
                            // Let's defer reset logic implementation for now or try simple approach
                            // Construct Vec<Vec<u8>> for all CPUs
                            if let Ok(num_cpus) = libbpf_rs::num_possible_cpus() {
                                let mut vals = Vec::new();
                                for _ in 0..num_cpus {
                                    vals.push(zero_bytes.to_vec());
                                }
                                let _ = skel.maps.stats_map.update_percpu(&key_bytes, &vals, MapFlags::ANY);
                            }
                            app.set_status("✓ Stats reset");
                        }
                        _ => {}
                    }
                }
            }
        }

        if last_tick.elapsed() >= tick_rate {
            last_tick = Instant::now();
        }
    }

    restore_terminal()?;
    Ok(())
}
