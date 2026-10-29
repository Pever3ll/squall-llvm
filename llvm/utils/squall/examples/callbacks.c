// Function pointers: dispatch tables, callbacks, function pointers stored in
// structures and globals, and recursion through a pointer.

typedef int (*binary)(int, int);
typedef int (*unary)(int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static int pick(int a, int b) { return a > b ? a : b; }

static const binary operations[] = {add, sub, mul, pick};

struct handler {
  const char *name;
  unary run;
  int weight;
};

static int twice(int x) { return 2 * x; }
static int negate(int x) { return -x; }
static int square(int x) { return x * x; }

static struct handler handlers[] = {
    {"twice", twice, 3}, {"negate", negate, 5}, {"square", square, 7}};

int dispatch(int op, int a, int b) { return operations[op & 3](a, b); }

static int fold(const int *values, int n, binary f, int initial) {
  int r = initial;
  for (int i = 0; i < n; ++i)
    r = f(r, values[i]);
  return r;
}

int folds(int seed) {
  int values[6];
  for (int i = 0; i < 6; ++i)
    values[i] = seed * (i + 1) - 7 * i;
  int total = 0;
  for (int op = 0; op < 4; ++op)
    total = total * 3 + fold(values, 6, operations[op], op);
  return total;
}

int handler_table(int x) {
  int total = 0;
  for (unsigned i = 0; i < sizeof handlers / sizeof handlers[0]; ++i)
    total += handlers[i].run(x + i) * handlers[i].weight + handlers[i].name[0];
  return total;
}

static void insertion_sort(int *a, int n, int (*before)(int, int)) {
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && before(a[j], a[j - 1]); --j) {
      int t = a[j];
      a[j] = a[j - 1];
      a[j - 1] = t;
    }
}

static int ascending(int a, int b) { return a < b; }
static int by_magnitude(int a, int b) {
  return (a < 0 ? -a : a) < (b < 0 ? -b : b);
}

int sort_with(int seed) {
  int a[9];
  for (int i = 0; i < 9; ++i)
    a[i] = (seed * (i * 7 + 3)) % 23 - 11;
  insertion_sort(a, 9, (seed & 1) ? ascending : by_magnitude);
  int s = 0;
  for (int i = 0; i < 9; ++i)
    s = s * 5 + a[i];
  return s;
}

static int countdown(unary step, int n) {
  return n <= 0 ? 0 : 1 + countdown(step, step(n));
}
static int minus_three(int n) { return n - 3; }
static int halve(int n) { return n / 2; }

int recursive_pointer(int n) {
  return countdown(minus_three, n) * 100 + countdown(halve, n);
}

static unary chooser(int which) {
  return which == 0 ? twice : which == 1 ? negate : square;
}

int returned_pointer(int which) {
  unary f = chooser(which % 3);
  unary g = chooser((which + 1) % 3);
  return f(g(which)) + (f == g) + 10 * (f == twice) + 100 * (chooser(2) == square);
}
