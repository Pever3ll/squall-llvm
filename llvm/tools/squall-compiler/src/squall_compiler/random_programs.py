"""Seeded, defined-behavior C workloads for an independent host oracle.

Inputs use n in [0, 8]. Arithmetic is unsigned, divisors are nonzero, shifts
are below 32, and every local cell is initialized before use. Each source is
saved alongside its LLVM IR and execution traces by the regression runner.
"""
import random


def generate(seed):
    rng = random.Random(seed)
    a, b, c = (rng.randrange(1, 1 << 32) for _ in range(3))
    shift = rng.randrange(32)
    divisor = rng.randrange(1, 65536)
    return f'''/* Reproduce with seed {seed}; valid n is 0..8. */
typedef unsigned int u32;
u32 kernel(u32 input, int n) {{
    u32 left[8], right[8];
    for (int i = 0; i < 8; ++i) {{
        left[i] = (input + (u32)i * {a}U) ^ {b}U;
        right[i] = (input ^ (u32)i) + {c}U;
    }}
    u32 result = input;
    for (int i = 0; i < n; ++i) {{
        u32 *p = (result & 1U) ? &left[i] : &right[i];
        u32 value = *p;
        if (value < result) result ^= (value >> {shift});
        else result += value / {divisor}U;
        *p = value + result;
        result = result * 33U + left[i] - right[i];
    }}
    return result;
}}
'''
