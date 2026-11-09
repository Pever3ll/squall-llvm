// Squall C library: the math functions that are not exact, from LLVM libc's
// header-only implementations (libc/shared/math.h): correctly rounded or
// within a few ulps, round to nearest, no floating-point exceptions. Built
// by build.py with LLVM libc's full-build headers.

// LLVM libc picks a floating-point environment type per architecture and
// has none for Squall, which has no floating-point environment; its generic
// code needs only the type.
#define LLVM_LIBC_TYPES_FENV_T_H
typedef struct {
  unsigned int unused;
} fenv_t;

#include "shared/math.h"

namespace math = LIBC_NAMESPACE::shared;

extern "C" {
double sin(double x) { return math::sin(x); }
double cos(double x) { return math::cos(x); }
double tan(double x) { return math::tan(x); }
double asin(double x) { return math::asin(x); }
double acos(double x) { return math::acos(x); }
double atan(double x) { return math::atan(x); }
double exp(double x) { return math::exp(x); }
double exp2(double x) { return math::exp2(x); }
double expm1(double x) { return math::expm1(x); }
double log(double x) { return math::log(x); }
double log2(double x) { return math::log2(x); }
double log10(double x) { return math::log10(x); }
double log1p(double x) { return math::log1p(x); }
double cbrt(double x) { return math::cbrt(x); }
double rint(double x) { return math::rint(x); }
double nearbyint(double x) { return math::nearbyint(x); }
double atan2(double x, double y) { return math::atan2(x, y); }
double pow(double x, double y) { return math::pow(x, y); }
double hypot(double x, double y) { return math::hypot(x, y); }
double fmod(double x, double y) { return math::fmod(x, y); }
double remainder(double x, double y) { return math::remainder(x, y); }
float sinf(float x) { return math::sinf(x); }
float cosf(float x) { return math::cosf(x); }
float tanf(float x) { return math::tanf(x); }
float asinf(float x) { return math::asinf(x); }
float acosf(float x) { return math::acosf(x); }
float atanf(float x) { return math::atanf(x); }
float expf(float x) { return math::expf(x); }
float exp2f(float x) { return math::exp2f(x); }
float expm1f(float x) { return math::expm1f(x); }
float logf(float x) { return math::logf(x); }
float log2f(float x) { return math::log2f(x); }
float log10f(float x) { return math::log10f(x); }
float log1pf(float x) { return math::log1pf(x); }
float cbrtf(float x) { return math::cbrtf(x); }
float rintf(float x) { return math::rintf(x); }
float nearbyintf(float x) { return math::nearbyintf(x); }
float sinhf(float x) { return math::sinhf(x); }
float coshf(float x) { return math::coshf(x); }
float tanhf(float x) { return math::tanhf(x); }
float atan2f(float x, float y) { return math::atan2f(x, y); }
float powf(float x, float y) { return math::powf(x, y); }
float hypotf(float x, float y) { return math::hypotf(x, y); }
float fmodf(float x, float y) { return math::fmodf(x, y); }
float remainderf(float x, float y) { return math::remainderf(x, y); }
double fma(double x, double y, double z) { return math::fma(x, y, z); }
float fmaf(float x, float y, float z) { return math::fmaf(x, y, z); }
double ldexp(double x, int e) { return math::ldexp(x, e); }
float ldexpf(float x, int e) { return math::ldexpf(x, e); }
double frexp(double x, int *e) { return math::frexp(x, e); }
float frexpf(float x, int *e) { return math::frexpf(x, e); }
double modf(double x, double *i) { return math::modf(x, i); }
float modff(float x, float *i) { return math::modff(x, i); }
}
