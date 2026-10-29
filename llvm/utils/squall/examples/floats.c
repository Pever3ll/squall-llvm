// Floating point in software: IEEE single and double precision with
// round-to-nearest-even, compared bit for bit with the host. NaN results are
// compared as NaN only, because NaN payloads are not specified.

#include <math.h>
#include <string.h>

typedef unsigned u32;
typedef unsigned long long u64;

static u32 fbits(float x) {
  u32 u;
  memcpy(&u, &x, sizeof u);
  return x != x ? 0x7fc00000u : u;
}
static u32 dbits(double x) {
  u64 u;
  memcpy(&u, &x, sizeof u);
  if (x != x)
    return 0x7ff80000u;
  return (u32)(u >> 32) * 31u ^ (u32)u;
}
static float fvalue(int a) { return (float)a / 7.0f; }
static double dvalue(int a) { return (double)a / 7.0; }

int float_arithmetic(int a, int b) {
  float x = fvalue(a), y = fvalue(b) * 1.5f + 0.25f;
  u32 r = fbits(x + y);
  r = r * 31u + fbits(x - y);
  r = r * 31u + fbits(x * y);
  r = r * 31u + fbits(x / y);
  r = r * 31u + fbits(-x);
  r = r * 31u + fbits(x * y + x); // Not contracted: rounded twice.
  return (int)r;
}

int double_arithmetic(int a, int b) {
  double x = dvalue(a), y = dvalue(b) * 1.5 + 0.25;
  u32 r = dbits(x + y);
  r = r * 31u + dbits(x - y);
  r = r * 31u + dbits(x * y);
  r = r * 31u + dbits(x / y);
  r = r * 31u + dbits(-x);
  r = r * 31u + dbits(x / 3.0 * y - x);
  return (int)r;
}

// Every comparison, including NaN (0/0) and signed zeros.
int comparisons(int a, int b) {
  float x = a == 1000 ? 0.0f / 0.0f : fvalue(a);
  double y = b == 1000 ? -0.0 : dvalue(b);
  float xf = (float)y;
  int r = (x < xf) | (x <= xf) << 1 | (x > xf) << 2 | (x >= xf) << 3 |
          (x == xf) << 4 | (x != xf) << 5 | (x != x) << 6;
  // The classification macros return any nonzero value for true.
  double xd = x;
  r |= (xd < y) << 8 | (xd <= y) << 9 | (xd > y) << 10 | (xd >= y) << 11 |
       (xd == y) << 12 | (xd != y) << 13 | !!isnan(xd) << 14 |
       (!(xd < y) && !(xd > y)) << 15 | !!signbit(y) << 16;
  return r;
}

// Conversions between every integer width and both floating types.
int conversions(int a) {
  float f = fvalue(a) * 1000.0f;
  double d = dvalue(a) * 1e12;
  u32 r = (u32)(int)f;
  r = r * 31u + (u32)(unsigned)(f < 0 ? -f : f);
  r = r * 31u + (u32)(long long)d;
  r = r * 31u + (u32)((long long)d >> 32);
  r = r * 31u + (u32)(unsigned long long)(d < 0 ? -d : d);
  r = r * 31u + (u32)(short)(int)(f / 100.0f) +
      (u32)(signed char)(int)(f / 1000.0f);
  r = r * 31u + fbits((float)a) + fbits((float)(unsigned)a);
  r = r * 31u + dbits((double)((long long)a * 1000003)) +
      dbits((double)(unsigned long long)((long long)a * (1LL << 33)));
  r = r * 31u + fbits((float)((long long)a * (1LL << 40)));
  r = r * 31u + dbits((double)f) + fbits((float)d);
  return (int)r;
}

int math(int a) {
  double x = dvalue(a) * 3.3;
  float y = (float)x;
  u32 r = dbits(sqrt(x < 0 ? -x : x));
  r = r * 31u + fbits(sqrtf(y < 0 ? -y : y));
  r = r * 31u + dbits(floor(x)) + dbits(ceil(x)) + dbits(trunc(x)) +
      dbits(round(x));
  r = r * 31u + fbits(floorf(y)) + fbits(ceilf(y)) + fbits(truncf(y)) +
      fbits(roundf(y));
  r = r * 31u + dbits(fmin(x, 1.5)) + dbits(fmax(x, -1.5)) +
      fbits(fminf(y, 0.0f));
  r = r * 31u + dbits(fabs(x)) + dbits(copysign(2.0, x)) + fbits(fabsf(y));
  r = r * 31u + dbits(sqrt(-1.0 + (a & 0))) + dbits(sqrt(0.0 * a));
  return (int)r;
}

// Newton's method converges to the correctly rounded root.
int newton(int a) {
  double target = (double)((a & 0xffff) + 2), x = target;
  for (int i = 0; i < 40; ++i)
    x = (x + target / x) * 0.5;
  return (int)(dbits(x) ^ dbits(sqrt(target)) * 3u);
}

// Floating-point globals and their initializers.
static float weights[4] = {0.5f, -1.25f, 3.0e-3f, 1.0e30f};
static double total = 1e-300;

int globals(int i) {
  weights[i & 3] *= 2.0f;
  total = total * 1e10 + weights[(i + 1) & 3];
  return (int)(fbits(weights[0] + weights[1] + weights[2] + weights[3]) ^
               dbits(total));
}

// Special values: infinities, the largest and smallest numbers, subnormals.
int specials(int which) {
  static const double values[] = {1e308,
                                  -1e308,
                                  2.2250738585072014e-308,
                                  4.9406564584124654e-324,
                                  3.4028234663852886e38,
                                  1.1754943508222875e-38,
                                  1.4e-45,
                                  0.1};
  double v = values[(unsigned)which % 8];
  float f = (float)v;
  u32 r = dbits(v * 10.0) + dbits(v / 1e300) * 3u + fbits(f * 2.0f) * 5u +
          fbits(f / 3.0f) * 7u + dbits((double)f * 0.5) * 11u +
          fbits((float)(v * 1e-30)) * 13u;
  return (int)r ^ !!isinf(v * 10.0) ^ !!isinf(f * 2.0f) << 1;
}
