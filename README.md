# scx_cake

A sched_ext scheduler inspired by the CAKE (Common Applications Kept Enhanced) bufferbloat algorithm. This scheduler applies CAKE's concepts to CPU scheduling for improved gaming and interactive performance.

## Performance

Early testing shows **excellent 1% lows** in games like Arc Raiders compared to default schedulers.

## Key Features

- **Sparse Flow Detection**: Automatically identifies and prioritizes interactive/gaming tasks based on runtime patterns
- **New-Flow Priority**: Waking tasks get priority dispatch (like CAKE's new-flow list)
- **Deficit Accounting**: Fair CPU time distribution based on runtime tracking
- **Priority Tiers**: Voice → Gaming → Best Effort → Background classification
- **Starvation Protection**: Prevents any task from monopolizing CPU time

## CAKE Concepts Adapted to CPU Scheduling

| CAKE (Network) | scx_cake (CPU) |
|----------------|----------------|
| Packet | Scheduling timeslice |
| Flow | Task/Process |
| Bandwidth | CPU time |
| Sparse flow | Interactive/gaming task |
| Bulk flow | CPU-bound task |
| Queue delay | Wake-to-run latency |

## Requirements

- Linux kernel 6.12+ with `CONFIG_SCHED_CLASS_EXT=y`
- Rust toolchain
- clang/LLVM for BPF compilation

## Building

```bash
./build.sh
```

## Running

```bash
# Run as root
sudo ./start.sh --verbose

# Or directly
sudo ./target/release/scx_cake --verbose
```

## Options

```
--quantum <µs>         Base scheduling quantum (default: 4000)
--new-flow-bonus <µs>  Bonus time for waking tasks (default: 8000)
--sparse-threshold     CPU usage threshold for sparse detection (default: 100‰)
--starvation-limit     Max time before forced preemption (default: 100000)
--verbose              Print statistics periodically
--interval <sec>       Stats print interval (default: 1)
```

## How It Works

1. **Task Wakeup**: Task is marked as "new flow" and gets priority
2. **Sparse Detection**: Short runtime bursts increase "sparse score"
3. **Tier Classification**: Nice value + sparse score determine priority tier
4. **Dispatch**: FIFO dispatch from shared DSQ with direct local dispatch for idle CPUs

## License

GPL-2.0
