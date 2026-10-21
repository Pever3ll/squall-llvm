/* Scalar kernels accepted at -O0 by the current LLVM-to-Squall lowering.
 * Unsigned arithmetic is used where wrapping is part of the expected result.
 */
int add(int a, int b) { return a + b; }

unsigned fanout(unsigned x) {
  unsigned a = x + x;
  unsigned b = x ^ 0x9e3779b9U;
  return (a * b) + x;
}

unsigned unsigned_divrem(unsigned a, unsigned b) {
  return (a / b) ^ (a % b);
}

int signed_divrem(int a, int b) { return a / b + a % b; }

unsigned shifts(unsigned a, unsigned b) { return (a >> b) ^ (a << b); }

int comparisons(int a, int b) {
  unsigned ua = (unsigned)a, ub = (unsigned)b;
  return (a < b) + 2 * (a <= b) + 4 * (a > b) + 8 * (a >= b)
       + 16 * (ua < ub) + 32 * (ua <= ub) + 64 * (ua > ub)
       + 128 * (ua >= ub) + 256 * (a == b) + 512 * (a != b);
}

int branch(int a, int b, int condition) {
  int result;
  if (condition) result = a;
  else result = b;
  return result;
}

int loop(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i) total += i;
  return total;
}

static int global_value = 3;
static void set_global(int value) { global_value = value; }

int memory_calls(int n) {
  int local = 4;
  global_value = 3;
  for (int i = 0; i < n; ++i) {
    int old = global_value;
    set_global(old + local);
  }
  return global_value;
}

int fibonacci(int n) {
  if (n < 2) return n;
  return fibonacci(n - 1) + fibonacci(n - 2);
}

static int combine(int a, int b, int c, int d) {
  return (a + b) * (c - d);
}

int calls(int a, int b, int c, int d) {
  int saved = a * 3;
  int first = combine(a, b, c, d);
  int second = combine(d, c, b, a);
  return first + second - saved;
}
