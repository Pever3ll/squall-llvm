// The inexact math functions, which come from LLVM libc. Results are
// returned as bits (doubles in two halves) so that the test can compare
// them with the host's libm within an ulp.

#include <math.h>
#include <string.h>

typedef unsigned long long u64;

static double argument(int a) {
  switch (a) {
  case 1000:
    return INFINITY;
  case 1001:
    return -INFINITY;
  case 1002:
    return NAN;
  case 1003:
    return -0.0;
  default:
    return a / 64.0;
  }
}

static double evaluate(int op, double x) {
  switch (op) {
  case 0:
    return sin(x);
  case 1:
    return cos(x);
  case 2:
    return tan(x);
  case 3:
    return asin(x / 2048.0);
  case 4:
    return acos(x / 2048.0);
  case 5:
    return atan(x);
  case 6:
    return exp(x / 16.0);
  case 7:
    return exp2(x / 16.0);
  case 8:
    return expm1(x / 16.0);
  case 9:
    return log(fabs(x) + 0.5);
  case 10:
    return log2(fabs(x) + 0.5);
  case 11:
    return log10(fabs(x) + 0.5);
  case 12:
    return log1p(fabs(x));
  case 13:
    return cbrt(x);
  case 14:
    return pow(fabs(x) + 0.25, 1.75);
  case 15:
    return atan2(x, 3.5);
  case 16:
    return hypot(x, 2.0);
  case 17:
    return fmod(x, 1.7);
  case 18:
    return remainder(x, 1.7);
  default:
    return rint(x * 3.5) + nearbyint(x / 3.0);
  }
}

static float evaluatef(int op, float x) {
  switch (op) {
  case 0:
    return sinf(x);
  case 1:
    return cosf(x);
  case 2:
    return tanf(x);
  case 3:
    return asinf(x / 2048.0f);
  case 4:
    return acosf(x / 2048.0f);
  case 5:
    return atanf(x);
  case 6:
    return expf(x / 16.0f);
  case 7:
    return exp2f(x / 16.0f);
  case 8:
    return expm1f(x / 16.0f);
  case 9:
    return logf(fabsf(x) + 0.5f);
  case 10:
    return log2f(fabsf(x) + 0.5f);
  case 11:
    return log10f(fabsf(x) + 0.5f);
  case 12:
    return log1pf(fabsf(x));
  case 13:
    return cbrtf(x);
  case 14:
    return powf(fabsf(x) + 0.25f, 1.75f);
  case 15:
    return atan2f(x, 3.5f);
  case 16:
    return hypotf(x, 2.0f);
  case 17:
    return sinhf(x / 8.0f);
  case 18:
    return tanhf(x / 8.0f);
  default:
    return coshf(x / 8.0f);
  }
}

static u64 bits(double x) {
  u64 u;
  memcpy(&u, &x, sizeof u);
  return u;
}

int math_high(int op, int a) {
  return (int)(bits(evaluate(op, argument(a))) >> 32);
}
int math_low(int op, int a) { return (int)bits(evaluate(op, argument(a))); }

int mathf_bits(int op, int a) {
  float r = evaluatef(op, (float)argument(a));
  int u;
  memcpy(&u, &r, sizeof u);
  return u;
}

// frexp, ldexp, and modf, with their pointer results folded in (finite
// arguments only: frexp's exponent is unspecified otherwise).
int split(int a) {
  double x = argument(a);
  int e;
  double m = frexp(x, &e), whole, part = modf(x * 3.25, &whole);
  float wf, pf = modff((float)x, &wf);
  int ef;
  float mf = frexpf((float)x, &ef);
  return (int)(bits(ldexp(m, e + 3)) >> 32) ^ (int)bits(part) ^
         (int)(bits(whole) >> 20) ^ e * 7 ^ ef * 13 ^ (int)(mf * 1024.0f) ^
         (int)(pf * 4096.0f) ^ (int)wf ^ (int)(ldexpf(mf, ef) * 64.0f);
}
