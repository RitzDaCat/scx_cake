# Hardware Compatibility

This document outlines design decisions and optimizations implemented in `scx_cake` to ensure high performance and stability across various consumer-grade Intel and AMD processors.

## Design Decisions

### Dynamic Loop Unrolling

We have removed explicit unroll counts from `#pragma unroll` directives throughout the BPF scheduler (e.g., in `cake_select_cpu` and `cake_dispatch`).

**Rationale:**
* **Architecture Agnostic**: Different CPU architectures (Zen 5, Alder Lake, etc.) have different pipeline depths and instruction caches. A fixed unroll factor that benefits a high-end CPU might cause instruction cache pressure or excessive code bloat on weaker or older processors.
* **Compiler Optimization**: By using `#pragma unroll` without a numeric argument, we allow the LLVM/BPF compiler to determine the optimal unroll factor based on the specific target architecture and loop complexity.
* **Portability**: This ensures that `scx_cake` maintains its low-latency characteristics without requiring manual tuning for every new CPU generation.
