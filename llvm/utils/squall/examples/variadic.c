// Variadic functions: unnamed arguments travel in the argument registers like
// named ones, and va_arg reads them from those registers' frame words.

#include <stdarg.h>
#include <string.h>

struct pair {
  int a, b;
};

static int sum(int count, ...) {
  va_list ap;
  va_start(ap, count);
  int total = 0;
  for (int i = 0; i < count; ++i)
    total += va_arg(ap, int);
  va_end(ap);
  return total;
}

// A tiny format interpreter: i int, l long long, d double, s string,
// p struct pair (by value), c char (promoted to int).
static long long format(const char *spec, ...) {
  va_list ap, again;
  va_start(ap, spec);
  va_copy(again, ap);
  long long result = 0;
  for (const char *p = spec; *p; ++p) {
    result *= 31;
    switch (*p) {
    case 'i':
      result += va_arg(ap, int);
      break;
    case 'l':
      result += va_arg(ap, long long) % 1000003;
      break;
    case 'd':
      result += (long long)(va_arg(ap, double) * 1000.0);
      break;
    case 's':
      result += (long long)strlen(va_arg(ap, const char *));
      break;
    case 'p': {
      struct pair v = va_arg(ap, struct pair);
      result += v.a * 7 + v.b;
      break;
    }
    case 'c':
      result += va_arg(ap, int);
      break;
    }
  }
  // The copy starts over at the first unnamed argument.
  if (*spec == 'i')
    result += va_arg(again, int) * 100;
  va_end(again);
  va_end(ap);
  return result;
}

int variadic(int n) {
  struct pair v = {n, -n * 2};
  char c = (char)('a' + (n & 7));
  long long r = sum(3, n, n * 2, n * 3) + sum(0) + sum(1, 42);
  r += format("ilsdpc", n, (long long)n * 34359738368LL, "squall", n / 3.0, v,
              c);
  r += format("d", 2.5) + format("");
  r += format("iiiiiiiiiiiiii", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, n);
  return (int)(r ^ r >> 32);
}

// A variadic function called through a pointer.
static int (*const indirect)(int, ...) = sum;

int variadic_pointer(int n) { return indirect(4, n, 1, 2, 3) * indirect(1, n); }
