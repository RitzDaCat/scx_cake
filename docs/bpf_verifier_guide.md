# BPF Verifier Best Practices

## Lesson Learned: Array Bounds Checking

### The Problem

```c
// ❌ BPF verifier REJECTS this
if (cpu < 64) {
    value = array[cpu];  // Verifier doesn't trust the comparison
}
```

The BPF verifier tracks value ranges conservatively. When `__builtin_ctzll()` is JIT-compiled, it uses a lookup table that returns 0-255. Even with an `if (cpu < 64)` check, the verifier may not propagate this constraint into the array access.

### The Solution

```c
// ✅ BPF verifier ACCEPTS this
u32 bounded_cpu = cpu & 63;  // Bitwise AND provably bounds to 0-63
value = array[bounded_cpu];
```

**Why it works:** The BPF verifier understands bitmask operations. `& 63` mathematically guarantees the result is 0-63, no matter the input value.

---

## General Rules for BSS Array Access

### 1. Always Use Bitmask for Bounds

```c
// For arrays of power-of-2 size
array[idx & (SIZE - 1)]  // e.g., & 63 for size 64
```

### 2. Declare Arrays with Known Size

```c
struct mystruct myarray[64] SEC(".bss") __attribute__((aligned(64)));
```

### 3. Validate Before Pointer Arithmetic

```c
// If creating a pointer, mask BEFORE pointer math
u32 safe_idx = raw_idx & 63;
struct mystruct *ptr = &myarray[safe_idx];
```

---

## Common Pitfalls

| Pattern                  | BPF Verifier Result                           |
| ------------------------ | --------------------------------------------- |
| `if (x < SIZE) array[x]` | ⚠️ May reject (comparison not always tracked) |
| `array[x & (SIZE-1)]`    | ✅ Always works (bitmask is provable)         |
| `array[x % SIZE]`        | ⚠️ Generates division (slow and may reject)   |

---

## Prevention Checklist

Before committing BPF changes:

1. [ ] All BSS array accesses use bitmask bounds
2. [ ] No naked `< SIZE` comparisons before array access
3. [ ] Test with `sudo ./start.sh` immediately after build
