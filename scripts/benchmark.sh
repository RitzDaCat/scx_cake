#!/bin/bash
# =============================================================================
# scx_cake Interactive Benchmark Suite
# =============================================================================
#
# A unified tool to benchmark scx_cake performance against baseline EEVDF.
# All functionality is self-contained.
#
# Usage: sudo ./scripts/benchmark.sh
#
# =============================================================================

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$SCRIPT_DIR/logs"
START_SCRIPT="$ROOT_DIR/start.sh"
PROJECT_DIR="$ROOT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# Check root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: Please run as root (sudo)${NC}"
    exit 1
fi

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# =============================================================================
# Data Collection Functions (Ported from monitor.sh)
# =============================================================================

get_bpf_stats() {
    bpftool prog show 2>/dev/null | awk '
    /struct_ops.*name cake_/ {
        for (i=1; i<=NF; i++) {
            if ($i == "name" && $(i+1) ~ /^cake_/) {
                gsub(/^cake_/, "", $(i+1))
                name = $(i+1)
            }
        }
    }
    /xlated.*jited/ {
        for (i=1; i<=NF; i++) {
            if ($i == "xlated") { xlated = $(i+1); gsub(/B$/, "", xlated) }
            if ($i == "jited") { jited = $(i+1); gsub(/B$/, "", jited) }
        }
    }
    /run_time_ns.*run_cnt/ {
        for (i=1; i<=NF; i++) {
            if ($i == "run_time_ns") run_time = $(i+1)
            if ($i == "run_cnt") run_cnt = $(i+1)
        }
        if (name != "") {
            print name, xlated, jited, run_cnt, run_time
            name = ""
        }
    }
    '
}

get_interrupt_stats() {
    python3 -c '
import sys
res, loc, tlb = 0, 0, 0
for line in open("/proc/interrupts"):
    parts = line.split()
    if not parts: continue
    label = parts[0]
    if label in ["RES:", "LOC:", "TLB:"]:
        # Sum only the numeric counters (skip label and description at end)
        count = sum(int(x) for x in parts[1:] if x.isdigit())
        if label == "RES:": res = count
        elif label == "LOC:": loc = count
        elif label == "TLB:": tlb = count
print(f"{res} {loc} {tlb}")
'
}

get_cs_stats() {
    awk '/^ctxt/ {cs=$2} /^intr/ {intr=$2} END {print cs, intr}' /proc/stat
}

get_load() {
    cat /proc/loadavg | awk '{print $1, $2, $3, $4}'
}

get_cpu_freq() {
    if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
        cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq 2>/dev/null | \
            awk '{sum+=$1; n++} END {if(n>0) print int(sum/n/1000); else print 0}'
    else
        echo 0
    fi
}

get_binary_size() {
    local binary="$PROJECT_DIR/target/release/scx_cake"
    if [ -f "$binary" ]; then
        stat -c%s "$binary"
    else
        echo 0
    fi
}

get_perf_stats() {
    if command -v perf &>/dev/null; then
        perf stat -x, -e instructions,cycles,branches,branch-misses,dTLB-loads,dTLB-load-misses,cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses \
            -a --timeout 1000 2>&1 | awk -F, '
            $3 == "instructions" { instr = $1 }
            $3 == "cycles" { cycles = $1 }
            $3 == "branches" { branches = $1 }
            $3 == "branch-misses" { branch_misses = $1 }
            $3 == "dTLB-loads" { tlb_loads = $1 }
            $3 == "dTLB-load-misses" { tlb_misses = $1 }
            $3 == "cache-references" { refs = $1 }
            $3 == "cache-misses" { misses = $1 }
            $3 == "L1-dcache-loads" { l1_loads = $1 }
            $3 == "L1-dcache-load-misses" { l1_misses = $1 }
            END {
                # Force numeric conversation
                instr += 0; cycles += 0; branches += 0; branch_misses += 0;
                tlb_loads += 0; tlb_misses += 0; refs += 0; misses += 0;
                l1_loads += 0; l1_misses += 0;

                ipc = 0; if (cycles > 0) ipc = instr / cycles;
                branch_miss_rate = 0; if (branches > 0) branch_miss_rate = (branch_misses / branches) * 100;
                tlb_miss_rate = 0; if (tlb_loads > 0) tlb_miss_rate = (tlb_misses / tlb_loads) * 100;
                llc_miss_rate = 0; if (refs > 0) llc_miss_rate = (misses / refs) * 100;
                l1d_miss_rate = 0; if (l1_loads > 0) l1d_miss_rate = (l1_misses / l1_loads) * 100;
                
                printf "%.2f %.2f %.2f %.2f %.2f\n", ipc, branch_miss_rate, tlb_miss_rate, llc_miss_rate, l1d_miss_rate
            }
        '
    else
        echo "0 0 0 0 0"
    fi
}

# =============================================================================
# Formatting Functions
# =============================================================================

print_menu_header() {
    clear
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║${NC}              ${BOLD}scx_cake Interactive Benchmark Suite${NC}                    ${BOLD}${CYAN}║${NC}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

print_monitor_header() {
    local interval=$1
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║${NC}              ${BOLD}scx_cake Performance Monitor${NC}                                    ${BOLD}${CYAN}║${NC}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════════════════════════╝${NC}"
    echo -e "${DIM}Interval: ${interval}s | $(date '+%Y-%m-%d %H:%M:%S') | Press Ctrl+C to stop${NC}"
    echo
}

# =============================================================================
# Internal Execution Engines
# =============================================================================

run_baseline_internal() {
    local duration=$1
    local name=$2
    local log_file=$3
    local scheduler="EEVDF (or current)"
    
    # Enable BPF stats just in case we need them later, though baseline mainly uses proc
    echo 1 > /proc/sys/kernel/bpf_stats_enabled 2>/dev/null || true

    # Header for log (JSON)
    printf '{"type": "header", "scheduler": "EEVDF", "mode": "%s", "duration": %d, "timestamp": "%s", "kernel": "%s", "host": "%s"}\n' \
        "$name" "$duration" "$(date -Iseconds)" "$(uname -r)" "$(hostname)" > "$log_file"
    
    local interval=2
    local end_time=$(($(date +%s) + duration))
    local sample=0
    
    # Read initial stats
    read prev_cs prev_intr <<< $(awk '/^ctxt/ {cs=$2} /^intr/ {intr=$2} END {print cs, intr}' /proc/stat)
    read prev_res prev_loc prev_tlb <<< $(get_interrupt_stats)
    
    # Arrays to collect perf samples
    declare -a ipc_samples branch_samples tlb_samples llc_samples l1d_samples

    # Background perf sampling every 1 second
    (
        while [ $(date +%s) -lt $end_time ]; do
            read ipc branch tlb llc l1d <<< $(get_perf_stats)
            echo "$ipc $branch $tlb $llc $l1d" >> "${log_file}.perf_samples"
            sleep 1
        done
    ) &
    local perf_pid=$!

    while [ $(date +%s) -lt $end_time ]; do
        sample=$((sample + 1))
        local remaining=$((end_time - $(date +%s)))
        [ $remaining -lt 0 ] && remaining=0
        
        sleep $interval
        
        # Get current stats
        local ts=$(date +%s)
        read cs intr <<< $(awk '/^ctxt/ {cs=$2} /^intr/ {intr=$2} END {print cs, intr}' /proc/stat)
        read res loc tlb <<< $(get_interrupt_stats)
        
        local cs_per_sec=$(( (cs - prev_cs) / interval ))
        local intr_per_sec=$(( (intr - prev_intr) / interval ))
        
        local res_per_sec=$(( (res - prev_res) / interval ))
        local loc_per_sec=$(( (loc - prev_loc) / interval ))
        local tlb_per_sec=$(( (tlb - prev_tlb) / interval ))
        
        prev_cs=$cs
        prev_intr=$intr
        prev_res=$res
        prev_loc=$loc
        prev_tlb=$tlb
        
        local freq=$(get_cpu_freq)
        read load1 load5 load15 runnable <<< $(get_load)

        # TUI Output
        echo -ne "\r${CYAN}[Sample $sample] ${remaining}s left - CS: ${cs_per_sec}/s  Intr: ${intr_per_sec}/s (RES: ${res_per_sec})  Freq: ${freq}MHz${NC}   "
        
        # JSON Output
        printf '{"type": "sample", "seq": %d, "timestamp": %d, "metrics": {"context_switches_sec": %d, "interrupts_sec": %d, "interrupts_breakdown": {"res": %d, "loc": %d, "tlb": %d}, "cpu_freq_mhz": %d, "load_avg": [%s, %s, %s], "runnable_tasks": "%s"}}\n' \
            "$sample" "$ts" "$cs_per_sec" "$intr_per_sec" "$res_per_sec" "$loc_per_sec" "$tlb_per_sec" "$freq" "$load1" "$load5" "$load15" "$runnable" >> "$log_file"
    done
    
    # Wait for perf sampling to complete
    wait $perf_pid 2>/dev/null || true
    
    echo ""
    echo -e "${CYAN}Calculating statistics from ${duration} perf samples...${NC}"
    
    # Calculate statistics from all samples
    if [ -f "${log_file}.perf_samples" ]; then
        read ipc_mean ipc_stddev ipc_min ipc_max \
             branch_mean branch_stddev branch_min branch_max \
             tlb_mean tlb_stddev tlb_min tlb_max \
             llc_mean llc_stddev llc_min llc_max \
             l1d_mean l1d_stddev l1d_min l1d_max \
             <<< $(awk '{
                ipc[NR]=$1; branch[NR]=$2; tlb[NR]=$3; llc[NR]=$4; l1d[NR]=$5;
                ipc_sum+=$1; branch_sum+=$2; tlb_sum+=$3; llc_sum+=$4; l1d_sum+=$5;
            } END {
                n=NR;
                ipc_avg=ipc_sum/n; branch_avg=branch_sum/n; tlb_avg=tlb_sum/n; llc_avg=llc_sum/n; l1d_avg=l1d_sum/n;
                
                ipc_min=ipc[1]; ipc_max=ipc[1];
                branch_min=branch[1]; branch_max=branch[1];
                tlb_min=tlb[1]; tlb_max=tlb[1];
                llc_min=llc[1]; llc_max=llc[1];
                l1d_min=l1d[1]; l1d_max=l1d[1];
                
                for(i=1;i<=n;i++) {
                    ipc_var += (ipc[i]-ipc_avg)^2;
                    branch_var += (branch[i]-branch_avg)^2;
                    tlb_var += (tlb[i]-tlb_avg)^2;
                    llc_var += (llc[i]-llc_avg)^2;
                    l1d_var += (l1d[i]-l1d_avg)^2;
                    
                    if(ipc[i]<ipc_min) ipc_min=ipc[i]; if(ipc[i]>ipc_max) ipc_max=ipc[i];
                    if(branch[i]<branch_min) branch_min=branch[i]; if(branch[i]>branch_max) branch_max=branch[i];
                    if(tlb[i]<tlb_min) tlb_min=tlb[i]; if(tlb[i]>tlb_max) tlb_max=tlb[i];
                    if(llc[i]<llc_min) llc_min=llc[i]; if(llc[i]>llc_max) llc_max=llc[i];
                    if(l1d[i]<l1d_min) l1d_min=l1d[i]; if(l1d[i]>l1d_max) l1d_max=l1d[i];
                }
                
                printf "%.2f %.2f %.2f %.2f ", ipc_avg, sqrt(ipc_var/n), ipc_min, ipc_max;
                printf "%.2f %.2f %.2f %.2f ", branch_avg, sqrt(branch_var/n), branch_min, branch_max;
                printf "%.2f %.2f %.2f %.2f ", tlb_avg, sqrt(tlb_var/n), tlb_min, tlb_max;
                printf "%.2f %.2f %.2f %.2f ", llc_avg, sqrt(llc_var/n), llc_min, llc_max;
                printf "%.2f %.2f %.2f %.2f\n", l1d_avg, sqrt(l1d_var/n), l1d_min, l1d_max;
            }' "${log_file}.perf_samples")
        
        rm -f "${log_file}.perf_samples"
    else
        # Fallback to single sample
        read ipc_mean branch_mean tlb_mean llc_mean l1d_mean <<< $(get_perf_stats)
        ipc_stddev=0; ipc_min=$ipc_mean; ipc_max=$ipc_mean;
        branch_stddev=0; branch_min=$branch_mean; branch_max=$branch_mean;
        tlb_stddev=0; tlb_min=$tlb_mean; tlb_max=$tlb_mean;
        llc_stddev=0; llc_min=$llc_mean; llc_max=$llc_mean;
        l1d_stddev=0; l1d_min=$l1d_mean; l1d_max=$l1d_mean;
    fi
    
    printf '{"type": "perf_stats", "ipc": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "branch_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "dTLB_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "llc_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "l1d_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}}\n' \
        "$ipc_mean" "$ipc_stddev" "$ipc_min" "$ipc_max" \
        "$branch_mean" "$branch_stddev" "$branch_min" "$branch_max" \
        "$tlb_mean" "$tlb_stddev" "$tlb_min" "$tlb_max" \
        "$llc_mean" "$llc_stddev" "$llc_min" "$llc_max" \
        "$l1d_mean" "$l1d_stddev" "$l1d_min" "$l1d_max" >> "$log_file"

    # Print summary to TUI
    echo -e "${GREEN}IPC: ${ipc_mean}±${ipc_stddev} (${ipc_min}-${ipc_max}) | Branch Miss: ${branch_mean}±${branch_stddev}% | LLC Miss: ${llc_mean}±${llc_stddev}%${NC}"
}

run_monitor_internal() {
    local duration=$1
    local name=$2
    local log_file=$3
    local interval=2
    
    # Enable BPF stats
    echo 1 > /proc/sys/kernel/bpf_stats_enabled 2>/dev/null || true

    # Header for log (JSON)
    printf '{"type": "header", "scheduler": "scx_cake", "mode": "%s", "duration": %d, "timestamp": "%s", "kernel": "%s", "host": "%s"}\n' \
        "$name" "$duration" "$(date -Iseconds)" "$(uname -r)" "$(hostname)" > "$log_file"

    # Internal state for deltas
    declare -A prev_bpf_cnt prev_bpf_time
    local prev_cs=0 prev_intr=0
    
    # Initialize baselines (first invisible run)
    local bpf_stats_init=$(get_bpf_stats)
    while IFS=' ' read -r n x j rc rt; do
        [ -z "$n" ] && continue
        prev_bpf_cnt[$n]=$rc
        prev_bpf_time[$n]=$rt
    done <<< "$bpf_stats_init"
    read prev_cs prev_intr <<< "$(get_cs_stats)"
    read prev_res prev_loc prev_tlb <<< "$(get_interrupt_stats)"
    
    local end_time=$(($(date +%s) + duration))
    
    # Background perf sampling every 1 second
    (
        while [ $(date +%s) -lt $end_time ]; do
            read ipc branch tlb llc l1d <<< $(get_perf_stats)
            echo "$ipc $branch $tlb $llc $l1d" >> "${log_file}.perf_samples"
            sleep 1
        done
    ) &
    local perf_pid=$!
    
    # Wait for first interval
    local samples=$((duration / interval))
    [ $samples -lt 1 ] && samples=1
    
    for ((i=1; i<=samples; i++)); do
        sleep $interval
        
        # Collect data
        local timestamp=$(date +%s)
        local bpf_stats=$(get_bpf_stats)
        read cs intr <<< "$(get_cs_stats)"
        read res loc tlb <<< "$(get_interrupt_stats)"
        read load1 load5 load15 runnable <<< "$(get_load)"
        local freq=$(get_cpu_freq)
        local binary_size=$(get_binary_size)

        # Calculate deltas
        local cs_per_sec=$(( (cs - prev_cs) / interval ))
        local intr_per_sec=$(( (intr - prev_intr) / interval ))
        local res_per_sec=$(( (res - prev_res) / interval ))
        local loc_per_sec=$(( (loc - prev_loc) / interval ))
        local tlb_per_sec=$(( (tlb - prev_tlb) / interval ))
        
        prev_cs=$cs
        prev_intr=$intr
        prev_res=$res
        prev_loc=$loc
        prev_tlb=$tlb
        
        # Prepare TUI Output - simplified for readability
        local buffer=""
        buffer+="\n${BOLD}System Metrics:${NC}\n"
        buffer+=$(printf "  CS: %d/s  |  Interrupts: %d/s  (RES: %d, LOC: %d)\n" "$cs_per_sec" "$intr_per_sec" "$res_per_sec" "$loc_per_sec")
        buffer+=$(printf "  CPU: %d MHz  |  Load: %.2f %.2f %.2f  |  Runnable: %s\n" "$freq" "$load1" "$load5" "$load15" "$runnable")
        
        buffer+="\n${BOLD}BPF Functions:${NC}\n"
        buffer+=$(printf "  %-14s %10s %10s %10s\n" "Function" "Calls/sec" "ns/call" "~Cycles")
        
        local total_overhead=0
        local total_jit=0
        local json_bpf_stats=""
        
        while IFS=' ' read -r name xlated jited run_cnt run_time; do
            [ -z "$name" ] && continue
            total_jit=$((total_jit + jited))
            
            local p_cnt=${prev_bpf_cnt[$name]:-$run_cnt}
            local p_time=${prev_bpf_time[$name]:-$run_time}
            
            local d_cnt=$((run_cnt - p_cnt))
            local d_time=$((run_time - p_time))
            
            local calls=0 ns_call=0 cycles=0
            if [ $d_cnt -gt 0 ]; then
                calls=$((d_cnt / interval))
                ns_call=$((d_time / d_cnt))
                cycles=$((ns_call * 5))
            fi
            
            total_overhead=$((total_overhead + (calls * ns_call)))
            
            local status="○ ok"
            local color=$YELLOW
            if [ $cycles -lt 100 ]; then status="✓ fast"; color=$GREEN; 
            elif [ $cycles -gt 500 ]; then status="✗ slow"; color=$RED; fi
            
            buffer+=$(printf "  ${CYAN}%-14s${NC} %10d %10d %10d\n" "$name" "$calls" "$ns_call" "$cycles")
            
            # Add to JSON string (comma separated)
            if [ -n "$json_bpf_stats" ]; then json_bpf_stats+=", "; fi
            json_bpf_stats+=$(printf '"%s": {"cnt": %d, "ns": %d, "cycles": %d}' "$name" "$calls" "$ns_call" "$cycles")
            
            prev_bpf_cnt[$name]=$run_cnt
            prev_bpf_time[$name]=$run_time
        done <<< "$bpf_stats"
        
        # Calculate overhead %
        local overhead_hundredths=$((total_overhead / 100000))
        local overhead_pct="$((overhead_hundredths / 100)).$((overhead_hundredths % 100))"
        
        buffer+="\n${BOLD}Scheduler Overhead: ${CYAN}${overhead_pct}%%${NC} of 1 core\n"
        
        # Display TUI
        print_monitor_header "$interval"
        echo -e "$buffer"
        
        # Log JSON
        printf '{"type": "sample", "seq": %d, "timestamp": %d, "metrics": {"context_switches_sec": %d, "interrupts_sec": %d, "interrupts_breakdown": {"res": %d, "loc": %d, "tlb": %d}, "cpu_freq_mhz": %d, "load_avg": [%s, %s, %s], "runnable_tasks": "%s"}, "bpf_stats": {%s}, "overhead_pct": %s}\n' \
            "$i" "$timestamp" "$cs_per_sec" "$intr_per_sec" "$res_per_sec" "$loc_per_sec" "$tlb_per_sec" "$freq" "$load1" "$load5" "$load15" "$runnable" "$json_bpf_stats" "$overhead_pct" >> "$log_file"
    done
    
    # Wait for perf sampling to complete
    wait $perf_pid 2>/dev/null || true
    
    echo -e "${CYAN}Calculating statistics from ${duration} perf samples...${NC}"
    
    # Calculate statistics from all samples
    if [ -f "${log_file}.perf_samples" ]; then
        read ipc_mean ipc_stddev ipc_min ipc_max \
             branch_mean branch_stddev branch_min branch_max \
             tlb_mean tlb_stddev tlb_min tlb_max \
             llc_mean llc_stddev llc_min llc_max \
             l1d_mean l1d_stddev l1d_min l1d_max \
             <<< $(awk '{
                ipc[NR]=$1; branch[NR]=$2; tlb[NR]=$3; llc[NR]=$4; l1d[NR]=$5;
                ipc_sum+=$1; branch_sum+=$2; tlb_sum+=$3; llc_sum+=$4; l1d_sum+=$5;
            } END {
                n=NR;
                ipc_avg=ipc_sum/n; branch_avg=branch_sum/n; tlb_avg=tlb_sum/n; llc_avg=llc_sum/n; l1d_avg=l1d_sum/n;
                
                ipc_min=ipc[1]; ipc_max=ipc[1];
                branch_min=branch[1]; branch_max=branch[1];
                tlb_min=tlb[1]; tlb_max=tlb[1];
                llc_min=llc[1]; llc_max=llc[1];
                l1d_min=l1d[1]; l1d_max=l1d[1];
                
                for(i=1;i<=n;i++) {
                    ipc_var += (ipc[i]-ipc_avg)^2;
                    branch_var += (branch[i]-branch_avg)^2;
                    tlb_var += (tlb[i]-tlb_avg)^2;
                    llc_var += (llc[i]-llc_avg)^2;
                    l1d_var += (l1d[i]-l1d_avg)^2;
                    
                    if(ipc[i]<ipc_min) ipc_min=ipc[i]; if(ipc[i]>ipc_max) ipc_max=ipc[i];
                    if(branch[i]<branch_min) branch_min=branch[i]; if(branch[i]>branch_max) branch_max=branch[i];
                    if(tlb[i]<tlb_min) tlb_min=tlb[i]; if(tlb[i]>tlb_max) tlb_max=tlb[i];
                    if(llc[i]<llc_min) llc_min=llc[i]; if(llc[i]>llc_max) llc_max=llc[i];
                    if(l1d[i]<l1d_min) l1d_min=l1d[i]; if(l1d[i]>l1d_max) l1d_max=l1d[i];
                }
                
                printf "%.2f %.2f %.2f %.2f ", ipc_avg, sqrt(ipc_var/n), ipc_min, ipc_max;
                printf "%.2f %.2f %.2f %.2f ", branch_avg, sqrt(branch_var/n), branch_min, branch_max;
                printf "%.2f %.2f %.2f %.2f ", tlb_avg, sqrt(tlb_var/n), tlb_min, tlb_max;
                printf "%.2f %.2f %.2f %.2f ", llc_avg, sqrt(llc_var/n), llc_min, llc_max;
                printf "%.2f %.2f %.2f %.2f\n", l1d_avg, sqrt(l1d_var/n), l1d_min, l1d_max;
            }' "${log_file}.perf_samples")
        
        rm -f "${log_file}.perf_samples"
    else
        # Fallback to single sample
        read ipc_mean branch_mean tlb_mean llc_mean l1d_mean <<< $(get_perf_stats)
        ipc_stddev=0; ipc_min=$ipc_mean; ipc_max=$ipc_mean;
        branch_stddev=0; branch_min=$branch_mean; branch_max=$branch_mean;
        tlb_stddev=0; tlb_min=$tlb_mean; tlb_max=$tlb_mean;
        llc_stddev=0; llc_min=$llc_mean; llc_max=$llc_mean;
        l1d_stddev=0; l1d_min=$l1d_mean; l1d_max=$l1d_mean;
    fi
    
    echo ""
    echo -e "${BOLD}${GREEN}Performance Stats (${duration} samples):${NC}"
    echo -e "  IPC: ${CYAN}${ipc_mean} ± ${ipc_stddev}${NC} (range: ${ipc_min}-${ipc_max})"
    echo -e "  Branch Miss: ${YELLOW}${branch_mean} ± ${branch_stddev}%${NC} (range: ${branch_min}-${branch_max}%)"
    echo -e "  LLC Miss: ${YELLOW}${llc_mean} ± ${llc_stddev}%${NC} (range: ${llc_min}-${llc_max}%)"
    echo -e "  L1D Miss: ${YELLOW}${l1d_mean} ± ${l1d_stddev}%${NC} (range: ${l1d_min}-${l1d_max}%)"
    
    printf '{"type": "perf_stats", "ipc": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "branch_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "dTLB_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "llc_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}, "l1d_miss_rate": {"mean": %s, "stddev": %s, "min": %s, "max": %s}}\n' \
        "$ipc_mean" "$ipc_stddev" "$ipc_min" "$ipc_max" \
        "$branch_mean" "$branch_stddev" "$branch_min" "$branch_max" \
        "$tlb_mean" "$tlb_stddev" "$tlb_min" "$tlb_max" \
        "$llc_mean" "$llc_stddev" "$llc_min" "$llc_max" \
        "$l1d_mean" "$l1d_stddev" "$l1d_min" "$l1d_max" >> "$log_file"
}


# =============================================================================
# Control Functions
# =============================================================================

get_scheduler() {
    local sched
    sched=$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || echo "")
    if [ -z "$sched" ]; then 
        echo "EEVDF"
    else 
        echo "scx_$sched"
    fi
}

stop_scx() {
    if pgrep scx_cake >/dev/null; then
        echo -e "${YELLOW}Stopping scx_cake...${NC}"
        pkill -SIGINT scx_cake || true
        sleep 2
        if pgrep scx_cake >/dev/null; then
            pkill -9 scx_cake || true
            sleep 1
        fi
    fi
}

start_scx() {
    if ! pgrep scx_cake >/dev/null; then
        echo -e "${GREEN}Starting scx_cake...${NC}"
        # Assuming start.sh is in parent dir
        if [ ! -f "$START_SCRIPT" ]; then
             echo -e "${RED}Error: start.sh not found at $START_SCRIPT${NC}"
             return 1
        fi
        
        nohup "$START_SCRIPT" >/dev/null 2>&1 &
        sleep 2
        if ! pgrep scx_cake >/dev/null; then
            echo -e "${RED}Failed to start scx_cake! Check logs.${NC}"
            read -p "Press Enter to continue..."
            return 1
        fi
        echo -e "${GREEN}scx_cake started successfully.${NC}"
    else
        echo -e "${GREEN}scx_cake is already running.${NC}"
    fi
    return 0
}

run_capture_wrapper() {
    local duration=$1
    local name=$2
    local scheduler=$(get_scheduler)
    local log_file="$LOG_DIR/${name}_${scheduler}_$(date +%Y%m%d_%H%M%S).jsonl"
    
    echo -e "${BOLD}Running capture for ${CYAN}${duration}s${NC} on ${YELLOW}${scheduler}${NC}..."
    echo -e "${DIM}Log: $log_file${NC}"
    
    if [[ "$scheduler" == "EEVDF" ]]; then
        run_baseline_internal "$duration" "$name" "$log_file"
    else
        run_monitor_internal "$duration" "$name" "$log_file"
    fi
    
    echo -e "${GREEN}Capture complete!${NC}"
    echo "LOG_FILE=$log_file" # Return log file path to caller
}

compare_logs() {
    local log1=$1
    local log2=$2
    
    echo -e "${BOLD}${MAGENTA}Comparison Summary:${NC}"
    echo -e "${DIM}Extracting key metrics (Context Switches, Overhead, Cache Misses)${NC}"
    echo ""
    
    echo -e "${BOLD}Baseline (EEVDF):${NC}"
    # Use grep/awk to parse lines from JSONL files roughly
    # Context switches (from samples - average)
    local cs_avg_1=$(awk '/"context_switches_sec":/ {sum+=$6; count++} END {if (count>0) print int(sum/count)}' "$log1" | tr -d ',')
    # Perf (from stats)
    local ipc_1=$(grep "ipc" "$log1" | sed 's/.*"ipc": \([0-9.]*\).*/\1/')
    local branch_miss_1=$(grep "branch_miss_rate" "$log1" | sed 's/.*"branch_miss_rate": \([0-9.]*\).*/\1/')
    local l1_miss_1=$(grep "l1d_miss_rate" "$log1" | sed 's/.*"l1d_miss_rate": \([0-9.]*\).*/\1/')
    local llc_miss_1=$(grep "llc_miss_rate" "$log1" | sed 's/.*"llc_miss_rate": \([0-9.]*\).*/\1/')
    
    local res_avg_1=$(awk '/"res":/ {sum+=$3; count++} END {if (count>0) print int(sum/count)}' "$log1" | tr -d ',')
    
    echo "  Context Switches: ${cs_avg_1}/sec"
    echo "  Resched Irqs:     ${res_avg_1}/sec"
    echo "  IPC:              ${ipc_1}"
    echo "  Branch Miss Rate: ${branch_miss_1}%"
    echo "  L1D Miss Rate:    ${l1_miss_1}%"
    echo "  LLC Miss Rate:    ${llc_miss_1}%"
    
    echo ""
    echo -e "${BOLD}scx_cake:${NC}"
    
    local cs_avg_2=$(awk '/"context_switches_sec":/ {sum+=$6; count++} END {if (count>0) print int(sum/count)}' "$log2" | tr -d ',')
    local overhead_2=$(awk '/"overhead_pct":/ {sum+=$NF; count++} END {if (count>0) print sum/count}' "$log2" | tr -d '}')
    local ipc_2=$(grep "ipc" "$log2" | sed 's/.*"ipc": \([0-9.]*\).*/\1/')
    local branch_miss_2=$(grep "branch_miss_rate" "$log2" | sed 's/.*"branch_miss_rate": \([0-9.]*\).*/\1/')
    local l1_miss_2=$(grep "l1d_miss_rate" "$log2" | sed 's/.*"l1d_miss_rate": \([0-9.]*\).*/\1/')
    local llc_miss_2=$(grep "llc_miss_rate" "$log2" | sed 's/.*"llc_miss_rate": \([0-9.]*\).*/\1/')
    
    local res_avg_2=$(awk '/"res":/ {sum+=$3; count++} END {if (count>0) print int(sum/count)}' "$log2" | tr -d ',')
    
    echo "  Context Switches: ${cs_avg_2}/sec"
    echo "  Resched Irqs:     ${res_avg_2}/sec"
    echo "  Scheduler Overhead: ${overhead_2}%"
    echo "  IPC:              ${ipc_2}"
    echo "  Branch Miss Rate: ${branch_miss_2}%"
    echo "  L1D Miss Rate:    ${l1_miss_2}%"
    echo "  LLC Miss Rate:    ${llc_miss_2}%"

    
    echo ""
}

# =============================================================================
# Main Menu
# =============================================================================

# Headless Mode Support
if [ "$1" == "--headless" ]; then
    workload_opt=$2
    target_opt=$3
    
    echo "Running in headless mode: Workload=$workload_opt Target=$target_opt"
    
    case $workload_opt in
        1) DURATION=5; SUFFIX="quick" ;;
        2) DURATION=15; SUFFIX="desktop" ;;
        3) DURATION=30; SUFFIX="gaming" ;;
        4) DURATION=60; SUFFIX="stress" ;;
        *) echo "Invalid headless workload"; exit 1 ;;
    esac
    
    case $target_opt in
        3) 
            start_scx || exit 1
            run_capture_wrapper "$DURATION" "$SUFFIX"
            exit 0
            ;;
        *) echo "Headless target not supported (only 3: scx_cake)"; exit 1 ;;
    esac
fi

while true; do
    print_menu_header
    CURRENT_SCHED=$(get_scheduler)
    echo -e "Current Scheduler: ${YELLOW}$CURRENT_SCHED${NC}"
    echo ""
    echo "Please select a benchmark workload:"
    echo -e "  ${BOLD}1)${NC} Quick Check    (5s)"
    echo -e "  ${BOLD}2)${NC} Desktop Usage  (15s)"
    echo -e "  ${BOLD}3)${NC} Gaming Session (30s)"
    echo -e "  ${BOLD}4)${NC} Stress Test    (60s)"
    echo -e "  ${BOLD}5)${NC} Custom Duration"
    echo -e "  ${BOLD}q)${NC} Quit"
    echo ""
    read -p "Select option: " workload_opt
    
    case $workload_opt in
        1) DURATION=5; SUFFIX="quick" ;;
        2) DURATION=15; SUFFIX="desktop" ;;
        3) DURATION=30; SUFFIX="gaming" ;;
        4) DURATION=60; SUFFIX="stress" ;;
        5) read -p "Enter duration (seconds): " DURATION; SUFFIX="custom" ;;
        q|Q) exit 0 ;;
        *) echo "Invalid option"; sleep 1; continue ;;
    esac
    
    echo ""
    echo "Select Benchmark Target:"
    echo -e "  ${BOLD}1)${NC} Current Scheduler (Run on $CURRENT_SCHED)"
    echo -e "  ${BOLD}2)${NC} Baseline Only (Force EEVDF)"
    echo -e "  ${BOLD}3)${NC} scx_cake Only (Force Load)"
    echo -e "  ${BOLD}4)${NC} Compare (Baseline vs scx_cake)"
    echo -e "  ${BOLD}b)${NC} Back"
    echo ""
    read -p "Select target: " target_opt
    
    case $target_opt in
        1) 
            run_capture_wrapper "$DURATION" "$SUFFIX"
            read -p "Press Enter to continue..."
            ;;
        2) 
            stop_scx
            run_capture_wrapper "$DURATION" "$SUFFIX"
            read -p "Press Enter to continue..."
            ;;
        3) 
            start_scx || continue
            run_capture_wrapper "$DURATION" "$SUFFIX"
            read -p "Press Enter to continue..."
            ;;
        4)
            echo -e "\n${BOLD}${MAGENTA}Phase 1: Baseline (EEVDF)${NC}"
            stop_scx
            echo -e "${YELLOW}Scheduler unloaded. Prepare for baseline capture in 3s...${NC}"
            sleep 3
            output1=$(run_capture_wrapper "$DURATION" "${SUFFIX}_baseline")
            log1=$(echo "$output1" | grep "LOG_FILE=" | cut -d= -f2 | tr -d '[:space:]')
            
            echo -e "\n${BOLD}${MAGENTA}Phase 2: scx_cake${NC}"
            start_scx || continue
            echo -e "${YELLOW}Scheduler loaded. Prepare for scx_cake capture in 3s...${NC}"
            sleep 3
            output2=$(run_capture_wrapper "$DURATION" "${SUFFIX}_scx_cake")
            log2=$(echo "$output2" | grep "LOG_FILE=" | cut -d= -f2 | tr -d '[:space:]')
            
            if [ -f "$log1" ] && [ -f "$log2" ]; then
                compare_logs "$log1" "$log2"
            else
                echo -e "${RED}Error: Could not find logs for comparison.${NC}"
            fi
            
            read -p "Press Enter to continue..."
            ;;
        b|B) continue ;;
        *) echo "Invalid option"; sleep 1; continue ;;
    esac
done
