/* Squall C library: the math functions that are exact. Floating point is
   implemented in software (IEEE single and double precision, round to
   nearest even, no exceptions). */
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

#ifdef __cplusplus
}
#endif

#endif
