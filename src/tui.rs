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
use crate::stats::TIER_NAMES;

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

    let mut output = String::new();
    output.push_str(&format!("=== scx_cake Statistics (Uptime: {}) ===\n\n", uptime));
    output.push_str(&format!("Dispatches: {} total ({:.1}% new-flow)\n\n", total_dispatches, new_pct));
    
    output.push_str("Tier           Dispatches    Max Wait    WaitDemote  StarvPreempt\n");
    output.push_str("─────────────────────────────────────────────────────────────────\n");
    for (i, name) in TIER_NAMES.iter().enumerate() {
        let max_wait_us = stats.max_wait_ns_tier[i] / 1000;
        output.push_str(&format!(
            "{:12}   {:>10}    {:>6} µs    {:>10}  {:>12}\n",
            name, stats.nr_tier_dispatches[i], max_wait_us,
            stats.nr_wait_demotions_tier[i], stats.nr_starvation_preempts_tier[i]
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
fn draw_ui(frame: &mut Frame, app: &TuiApp, stats: &cake_stats) {
    let area = frame.area();

    // Create main layout: header, stats table, footer
    let layout = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),  // Header
            Constraint::Min(10),    // Stats table
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

    // --- Stats Table ---
    let header_cells = ["Tier", "Dispatches", "Max Wait", "WaitDemote", "StarvPreempt"]
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
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(12),
            Constraint::Length(14),
        ],
    )
    .header(header_row)
    .block(Block::default()
        .title(" Per-Tier Statistics ")
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Blue)));
    frame.render_widget(table, layout[1]);

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
    frame.render_widget(summary, layout[2]);

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
    frame.render_widget(footer, layout[3]);
}

/// Get color style for a tier
fn tier_style(tier: usize) -> Style {
    match tier {
        0 => Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD),   // CritLatency - highest priority
        1 => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),    // Realtime
        2 => Style::default().fg(Color::Magenta),                              // Critical
        3 => Style::default().fg(Color::Green),                                // Gaming
        4 => Style::default().fg(Color::Yellow),                               // Interactive
        5 => Style::default().fg(Color::Blue),                                 // Batch
        6 => Style::default().fg(Color::DarkGray),                             // Background
        _ => Style::default(),
    }
}

/// Run the TUI event loop
pub fn run_tui(
    skel: &mut BpfSkel,
    shutdown: Arc<AtomicBool>,
    interval_secs: u64,
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

        // Get current stats
        let stats = match &skel.maps.bss_data {
            Some(bss) => bss.stats.clone(),
            None => continue,
        };

        // Draw UI
        terminal.draw(|frame| draw_ui(frame, &app, &stats))?;

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
                            if let Some(bss) = &mut skel.maps.bss_data {
                                let stats_mut = &mut bss.stats;
                                stats_mut.total_wait_ns = 0;
                                stats_mut.nr_waits = 0;
                                stats_mut.max_wait_ns = 0;
                                stats_mut.nr_input_preempts = 0;
                                for i in 0..7 {  // 7 tiers: CritLatency, Realtime, Critical, Gaming, Interactive, Batch, Background
                                    stats_mut.total_wait_ns_tier[i] = 0;
                                    stats_mut.nr_waits_tier[i] = 0;
                                    stats_mut.max_wait_ns_tier[i] = 0;
                                }
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
