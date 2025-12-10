# scx_cake

> ⚠️ **EXPERIMENTAL** - This scheduler is under active development. Use at your own risk.

A simple sched_ext scheduler inspired by CAKE bufferbloat concepts. Currently implements direct dispatch optimization for low-latency gaming.

## Performance

Early testing showed good framerate and 1% lows in games like Arc Raiders.

## What It Actually Does (Current Version)

This is a **simplified scheduler** focused on one key optimization:

### Direct Local Dispatch
When a task wakes up and there's an idle CPU available, it runs **immediately** without going through any queue. This is the main source of latency improvement.

### FIFO Queue
All other tasks go to a single shared FIFO queue - simple and fair.

### Starvation Protection  
Tasks running too long get preempted (configurable via `--starvation-limit`).

## What's NOT Implemented Yet

The following CAKE-inspired features are **tracked for statistics only** and don't affect scheduling:

- ❌ Sparse flow detection (calculated but unused)
- ❌ Priority tiers (classified but all go to same queue)
- ❌ Quantum-based time slicing (uses kernel default)
- ❌ Multi-DSQ new-flow/old-flow priority

These features are planned for future versions.

## Code Structure

```
scx_cake/
├── Cargo.toml              # Rust dependencies
├── build.rs                # BPF compilation (uses scx_cargo)
├── src/
│   ├── main.rs             # Userspace: CLI, BPF loading, stats
│   ├── stats.rs            # Statistics utilities
│   └── bpf/
│       ├── intf.h          # Shared types (BPF ↔ Rust)
│       └── cake.bpf.c      # Kernel BPF scheduler logic
├── build.sh                # Build helper script
└── start.sh                # Run helper script
```

## Configuration

### Options That Work

| Option | Default | Description |
|--------|---------|-------------|
| `--quantum` | 4000 µs | Time slice for each task |
| `--starvation-limit` | 100000 µs | Max time before forced preemption |
| `--verbose` | false | Print statistics periodically |
| `--interval` | 1 sec | Stats print interval |

### Options That Don't Work Yet

These are parsed but don't affect scheduling behavior:

| Option | Default | Status |
|--------|---------|--------|
| `--sparse-threshold` | 100‰ | ❌ Not used |
| `--new-flow-bonus` | 8000 µs | ❌ Not used |

## Requirements

- **Kernel**: Linux 6.12+ with `CONFIG_SCHED_CLASS_EXT=y`
- **Rust**: Stable toolchain
- **Clang/LLVM**: For BPF compilation

## Building

```bash
./build.sh
```

## Running

```bash
# Run with verbose stats
sudo ./start.sh --verbose

# Or directly
sudo ./target/release/scx_cake --verbose
```

## Why It Works

The direct local dispatch optimization is surprisingly effective for gaming:

1. Game thread sleeps (waiting for vsync, input, etc.)
2. Event occurs (vsync, mouse move, etc.)
3. Game thread wakes up
4. If any CPU is idle → **runs immediately** (no queue wait)
5. Game processes the event with minimal latency

On a system with spare CPU capacity (common for gaming), this means interactive tasks almost always get immediate execution.

## Future Plans

- Implement actual multi-tier DSQs with priority dispatch
- Make quantum control the actual time slice
- Use sparse detection to prioritize interactive tasks

## License

GPL-2.0

## Contributing

This is an experimental scheduler. Contributions, testing, and feedback welcome!
