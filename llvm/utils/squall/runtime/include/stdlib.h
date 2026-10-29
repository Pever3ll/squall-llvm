/* Squall C library: general utilities. There is no heap, environment, or
   process control; abort() halts the program with the trap status. */
#ifndef SQUALL_STDLIB_H
#define SQUALL_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int quot, rem;
} div_t;
typedef struct {
  long quot, rem;
} ldiv_t;
typedef struct {
  long long quot, rem;
} lldiv_t;

#define RAND_MAX 0x7fffffff

int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
long strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
long long strtoll(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base);

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

int rand(void);
void srand(unsigned seed);

_Noreturn void abort(void);

#ifdef __cplusplus
}
#endif

#endif
