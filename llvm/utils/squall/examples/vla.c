// Variable-length arrays and alloca: allocated from the Squall alloca area
// and released when their scope (or function) ends.

#include <string.h>

int vla_sum(int n) {
  n = (n < 0 ? -n : n) % 60 + 1;
  int values[n];
  for (int i = 0; i < n; ++i)
    values[i] = i * i - n;
  int total = 0;
  for (int i = n - 1; i >= 0; --i)
    total = total * 3 + values[i];
  return total;
}

// A new array on every iteration: each is released at the end of its scope,
// so the area does not run out however many iterations there are.
int vla_loop(int n) {
  int total = 0;
  for (int k = 0; k < 200; ++k) {
    char buffer[(n & 15) + k % 7 + 1];
    memset(buffer, k, sizeof buffer);
    total += buffer[sizeof buffer - 1] + (int)sizeof buffer;
  }
  return total;
}

// Every activation has its own array.
static int depth(int n) {
  if (n <= 0)
    return 0;
  short local[n];
  for (int i = 0; i < n; ++i)
    local[i] = (short)(n * 100 + i);
  int below = depth(n - 1);
  return below * 7 + local[n - 1] - local[0];
}

int vla_recursive(int n) { return depth((n & 7) + 1); }

int alloca_bytes(int n) {
  n = (n & 31) + 1;
  unsigned char *p = __builtin_alloca(n);
  for (int i = 0; i < n; ++i)
    p[i] = (unsigned char)(i * 37 + n);
  unsigned total = 0;
  for (int i = 0; i < n; ++i)
    total = total * 131 + p[i];
  return (int)total;
}

// Larger than the alloca area: the program traps.
int vla_overflow(int n) {
  int big[n];
  big[0] = n;
  big[n - 1] = 2;
  return big[0] + big[n - 1];
}
