// Squall C library: math functions with exact results. Arithmetic on float
// and double is done by the compiler-rt soft-float builtins.

#include <math.h>
#include <stdint.h>

static uint64_t bits(double x) {
  union {
    double d;
    uint64_t u;
  } v = {x};
  return v.u;
}
static double from_bits(uint64_t u) {
  union {
    uint64_t u;
    double d;
  } v = {u};
  return v.d;
}
static uint32_t bitsf(float x) {
  union {
    float f;
    uint32_t u;
  } v = {x};
  return v.u;
}
static float from_bitsf(uint32_t u) {
  union {
    uint32_t u;
    float f;
  } v = {u};
  return v.f;
}

#define SIGN 0x8000000000000000ULL
#define SIGNF 0x80000000U

double fabs(double x) { return from_bits(bits(x) & ~SIGN); }
float fabsf(float x) { return from_bitsf(bitsf(x) & ~SIGNF); }
double copysign(double x, double y) {
  return from_bits((bits(x) & ~SIGN) | (bits(y) & SIGN));
}
float copysignf(float x, float y) {
  return from_bitsf((bitsf(x) & ~SIGNF) | (bitsf(y) & SIGNF));
}

double trunc(double x) {
  uint64_t u = bits(x);
  int e = (int)(u >> 52 & 0x7ff) - 1023;
  if (e >= 52)
    return x; // Integral, infinite, or NaN.
  if (e < 0)
    return from_bits(u & SIGN);
  return from_bits(u & ~((1ULL << (52 - e)) - 1));
}
float truncf(float x) {
  uint32_t u = bitsf(x);
  int e = (int)(u >> 23 & 0xff) - 127;
  if (e >= 23)
    return x;
  if (e < 0)
    return from_bitsf(u & SIGNF);
  return from_bitsf(u & ~((1U << (23 - e)) - 1));
}

double floor(double x) {
  double t = trunc(x);
  return t > x ? t - 1.0 : t;
}
float floorf(float x) {
  float t = truncf(x);
  return t > x ? t - 1.0f : t;
}
double ceil(double x) {
  double t = trunc(x);
  return t < x ? t + 1.0 : t;
}
float ceilf(float x) {
  float t = truncf(x);
  return t < x ? t + 1.0f : t;
}
// Halfway cases round away from zero. x - trunc(x) is exact.
double round(double x) {
  double t = trunc(x);
  return fabs(x - t) >= 0.5 ? t + copysign(1.0, x) : t;
}
float roundf(float x) {
  float t = truncf(x);
  return fabsf(x - t) >= 0.5f ? t + copysignf(1.0f, x) : t;
}

// A NaN operand yields the other operand; -0 orders below +0.
double fmin(double x, double y) {
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? x : y;
  return x < y ? x : y;
}
float fminf(float x, float y) {
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? x : y;
  return x < y ? x : y;
}
double fmax(double x, double y) {
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? y : x;
  return x > y ? x : y;
}
float fmaxf(float x, float y) {
  if (isnan(x))
    return y;
  if (isnan(y))
    return x;
  if (x == y)
    return signbit(x) ? y : x;
  return x > y ? x : y;
}

// Correctly rounded: the digit-by-digit integer square root of the
// mantissa, scaled so the root has 54 bits (53 and a rounding bit).
double sqrt(double x) {
  uint64_t u = bits(x);
  if ((u & ~SIGN) == 0 || (u >> 52) == 0x7ff)
    return x; // +-0, +inf, or a positive NaN.
  if (u & SIGN)
    return (x - x) / (x - x); // Negative or a negative NaN: NaN.
  // x = m * 2^e with m normalized to 53 bits.
  uint64_t m = u & ((1ULL << 52) - 1);
  int e;
  if (u >> 52) {
    m |= 1ULL << 52;
    e = (int)(u >> 52) - 1075;
  } else {
    e = -1074;
    while (!(m >> 52)) {
      m <<= 1;
      --e;
    }
  }
  if (e & 1) {
    m <<= 1;
    --e;
  }
  // m is in [2^52, 2^54), so floor(sqrt(m * 2^54)) is in [2^53, 2^54). The
  // remainder stays below 2^56; the radicand's low 54 bits are zero.
  uint64_t q = 0, rem = 0;
  for (int i = 53; i >= 0; --i) {
    unsigned pair = 2 * i >= 54 ? (unsigned)(m >> (2 * i - 54)) & 3 : 0;
    rem = rem << 2 | pair;
    uint64_t t = q << 2 | 1;
    q <<= 1;
    if (rem >= t) {
      rem -= t;
      q |= 1;
    }
  }
  uint64_t mantissa = q >> 1;
  // A root is never exactly halfway between doubles; ties are even anyway.
  if ((q & 1) && (rem || (mantissa & 1)))
    ++mantissa;
  int exponent = e / 2 + 26 + 1023;
  if (mantissa >> 53) {
    mantissa >>= 1;
    ++exponent;
  }
  return from_bits((uint64_t)exponent << 52 | (mantissa & ((1ULL << 52) - 1)));
}

// Rounding the double root to float is correctly rounded, because double has
// more than twice float's precision.
float sqrtf(float x) { return (float)sqrt((double)x); }
