/* Squall C library: math functions. Floating point is implemented in
   software (IEEE single and double precision, round to nearest even, no
   exceptions). The exact functions are the library's own; the others come
   from LLVM libc. Not provided: sinh, cosh, and tanh for double, and the
   long double, error, and gamma functions. */
#ifndef SQUALL_MATH_H
#define SQUALL_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double x);
float fabsf(float x);
double copysign(double x, double y);
float copysignf(float x, float y);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double trunc(double x);
float truncf(float x);
double round(double x);
float roundf(float x);
double fmin(double x, double y);
float fminf(float x, float y);
double fmax(double x, double y);
float fmaxf(float x, float y);
double sqrt(double x);
float sqrtf(float x);

double sin(double x);
float sinf(float x);
double cos(double x);
float cosf(float x);
double tan(double x);
float tanf(float x);
double asin(double x);
float asinf(float x);
double acos(double x);
float acosf(float x);
double atan(double x);
float atanf(float x);
double exp(double x);
float expf(float x);
double exp2(double x);
float exp2f(float x);
double expm1(double x);
float expm1f(float x);
double log(double x);
float logf(float x);
double log2(double x);
float log2f(float x);
double log10(double x);
float log10f(float x);
double log1p(double x);
float log1pf(float x);
double cbrt(double x);
float cbrtf(float x);
double rint(double x);
float rintf(float x);
double nearbyint(double x);
float nearbyintf(float x);
double atan2(double x, double y);
float atan2f(float x, float y);
double pow(double x, double y);
float powf(float x, float y);
double hypot(double x, double y);
float hypotf(float x, float y);
double fmod(double x, double y);
float fmodf(float x, float y);
double remainder(double x, double y);
float remainderf(float x, float y);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
double ldexp(double x, int e);
float ldexpf(float x, int e);
double frexp(double x, int *e);
float frexpf(float x, int *e);
double modf(double x, double *i);
float modff(float x, float *i);

#ifdef __cplusplus
}
#endif

#endif
