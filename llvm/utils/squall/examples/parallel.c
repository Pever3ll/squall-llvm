// Kernels with independent work inside each loop iteration, for
// -mllvm -squall-fork-join: the backend runs the independent instruction
// groups as two tokens, which overlap on pipelined cores.

typedef unsigned u32;

// Two pseudo-random chains: no memory, no calls.
int chains(int n) {
  u32 a = (u32)n, b = (u32)n ^ 0x5555u;
  for (int i = 0; i < n; ++i) {
    a = a * 1664525u + 1013904223u;
    a ^= a >> 13;
    a += a << 3;
    b = b * 22695477u + 1u;
    b ^= b >> 7;
    b += b << 5;
  }
  return (int)(a ^ b);
}

// Two polynomials evaluated by Horner's rule at the same point.
int horner(int x) {
  static const int p[8] = {3, -1, 4, 1, -5, 9, -2, 6};
  static const int q[8] = {2, 7, -1, 8, 2, -8, 1, 8};
  int ux = x % 97;
  u32 a = 0, b = 0;
  for (int i = 0; i < 8; ++i) {
    a = a * (u32)ux + (u32)p[i];
    b = b * (u32)(ux + 1) + (u32)q[i];
  }
  return (int)(a - b);
}

// A memory lane (a running sum over an array) beside an arithmetic lane.
int sum_and_hash(int n) {
  int values[16];
  for (int i = 0; i < 16; ++i)
    values[i] = n * i - 7;
  int sum = 0;
  u32 h = 2166136261u;
  for (int i = 0; i < 16; ++i) {
    sum += values[i];
    h = (h ^ (u32)i) * 16777619u;
    h ^= h >> 11;
  }
  return sum ^ (int)h;
}

// A call in one lane beside arithmetic in the other. The callee cannot call
// back into this function, so the region joins on a reserved word of its own
// while the callee runs (and may fork and join itself).
__attribute__((noinline)) static int collatz_steps(int n) {
  int steps = 0;
  while (n != 1 && steps < 500) {
    n = n & 1 ? 3 * n + 1 : n / 2;
    ++steps;
  }
  return steps;
}

int call_beside_arithmetic(int n) {
  int steps = collatz_steps((n & 1023) | 1);
  u32 h = (u32)n * 2654435761u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  h *= 3266489917u;
  h ^= h >> 16;
  h += 0x9e3779b9u;
  h ^= h << 6;
  return steps ^ (int)h;
}

// Frame arrays in both lanes: static frame slots need no memory helper, and
// the two arrays cannot alias.
int two_frame_arrays(int n) {
  volatile int a[4], b[4];
  a[0] = n;
  a[1] = n + 1;
  a[2] = a[0] * a[1];
  a[3] = a[2] - n;
  b[0] = n ^ 0x55;
  b[1] = b[0] * 7;
  b[2] = b[1] + b[0];
  b[3] = b[2] >> 2;
  return a[3] + b[3];
}
