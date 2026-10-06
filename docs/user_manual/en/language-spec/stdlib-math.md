# math — 25 functions

The floating-point function family all runs at **double** precision:
parameters and return values are `double`; integer and
`float` arguments enter via implicit widening (`math.sqrt(4)` and
`math.sin(1.5f)` both compile).
`sin`/`cos`/`tan` take radians; `asin`/`acos`/`atan` return radians.
`log` is the natural logarithm.
`floor`/`ceil`/`round` return **long** (`round` is half-away-from-zero).

| Function | Signature | Notes |
|----------|-----------|-------|
| sin cos tan asin acos atan | (double) → double | radians |
| atan2 | (double y, double x) → double | C/C++ argument order |
| sqrt pow exp log | (double[,double]) → double | pow(x,y); log = ln |
| absi / absf | (int)→int / (double)→double | absi(int minimum) throws |
| mini maxi / minf maxf | (T, T) → T | int pair / double pair |
| clampi / clampf | (v, lo, hi) → T | lo > hi → Exception |
| floor ceil round | (double) → long | out-of-int64 or NaN → Exception |
| random | () → double | [0,1), pseudorandom number generator (PRNG) below |
| srand | (int) → void | reseeds |
| randomi | (int min, int max) → int | inclusive bounds; min > max → Exception |

**PRNG determinism**: `std::mt19937`, seeded from `std::random_device` at
program start. `math.srand(n)` reseeds explicitly — after it, sequences are
fully deterministic and identical across platforms:

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24-bit mantissa, exact in [0,1)
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // small modular bias, documented
```

No `std::uniform_*_distribution` is used — those are implementation-defined
and not portable.
