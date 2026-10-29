// Squall C library: general utilities.

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int errno;

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

div_t div(int n, int d) { return (div_t){n / d, n % d}; }
ldiv_t ldiv(long n, long d) { return (ldiv_t){n / d, n % d}; }
lldiv_t lldiv(long long n, long long d) { return (lldiv_t){n / d, n % d}; }

// Converts the digits after an optional sign and base prefix. Values are
// accumulated unsigned and clamped to Limit, the magnitude bound of the
// result type; *Negative reports the sign.
static unsigned long long convert(const char *s, char **end, int base,
                                  unsigned long long limit, int *negative) {
  const char *p = s;
  while (isspace(*p))
    ++p;
  *negative = 0;
  if (*p == '+' || *p == '-')
    *negative = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' &&
      isxdigit(p[2])) {
    p += 2;
    base = 16;
  } else if (base == 0) {
    base = *p == '0' ? 8 : 10;
  }
  if (base < 2 || base > 36) {
    errno = EINVAL;
    if (end)
      *end = (char *)s;
    return 0;
  }
  unsigned long long value = 0;
  int any = 0, overflow = 0;
  for (;; ++p) {
    int digit;
    if (isdigit(*p))
      digit = *p - '0';
    else if (isalpha(*p))
      digit = (*p | 32) - 'a' + 10;
    else
      break;
    if (digit >= base)
      break;
    any = 1;
    if (value > (limit - (unsigned)digit) / (unsigned)base)
      overflow = 1;
    else
      value = value * (unsigned)base + (unsigned)digit;
  }
  if (end)
    *end = (char *)(any ? p : s);
  if (overflow) {
    errno = ERANGE;
    return limit;
  }
  return value;
}

long long strtoll(const char *restrict s, char **restrict end, int base) {
  int negative;
  unsigned long long v =
      convert(s, end, base, (unsigned long long)LLONG_MAX + 1, &negative);
  if (!negative && v > LLONG_MAX) {
    errno = ERANGE;
    return LLONG_MAX;
  }
  return negative ? (long long)(0 - v) : (long long)v;
}

unsigned long long strtoull(const char *restrict s, char **restrict end,
                            int base) {
  int negative;
  unsigned long long v = convert(s, end, base, ULLONG_MAX, &negative);
  return negative ? 0 - v : v;
}

long strtol(const char *restrict s, char **restrict end, int base) {
  int negative;
  unsigned long long v =
      convert(s, end, base, (unsigned long long)LONG_MAX + 1, &negative);
  if (!negative && v > LONG_MAX) {
    errno = ERANGE;
    return LONG_MAX;
  }
  return negative ? (long)(0 - v) : (long)v;
}

unsigned long strtoul(const char *restrict s, char **restrict end, int base) {
  int negative;
  unsigned long long v = convert(s, end, base, ULONG_MAX, &negative);
  return (unsigned long)(negative ? 0 - v : v);
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

static void swap(unsigned char *a, unsigned char *b, size_t size) {
  while (size--) {
    unsigned char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

// Heapsort: in place, without recursion, and O(n log n) in the worst case.
static void sift(unsigned char *base, size_t root, size_t count, size_t size,
                 int (*compare)(const void *, const void *)) {
  for (;;) {
    size_t child = 2 * root + 1;
    if (child >= count)
      return;
    if (child + 1 < count &&
        compare(base + child * size, base + (child + 1) * size) < 0)
      ++child;
    if (compare(base + root * size, base + child * size) >= 0)
      return;
    swap(base + root * size, base + child * size, size);
    root = child;
  }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
  unsigned char *b = base;
  for (size_t i = count / 2; i-- > 0;)
    sift(b, i, count, size, compare);
  for (size_t n = count; n > 1; --n) {
    swap(b, b + (n - 1) * size, size);
    sift(b, 0, n - 1, size, compare);
  }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
  const unsigned char *b = base;
  while (count) {
    const unsigned char *middle = b + (count / 2) * size;
    int order = compare(key, middle);
    if (!order)
      return (void *)middle;
    if (order > 0) {
      b = middle + size;
      count -= count / 2 + 1;
    } else {
      count /= 2;
    }
  }
  return NULL;
}

// A 64-bit linear congruential generator (Knuth's MMIX constants).
static unsigned long long next = 1;

int rand(void) {
  next = next * 6364136223846793005ULL + 1442695040888963407ULL;
  return (int)(next >> 33);
}

void srand(unsigned seed) { next = seed; }

_Noreturn void abort(void) { __builtin_trap(); }
