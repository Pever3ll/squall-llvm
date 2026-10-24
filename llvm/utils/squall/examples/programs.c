/* General C control flow and data structures on 32-bit cells. */
struct point {
  int x, y;
};

static struct point scale(struct point p, int k) {
  struct point r = {p.x * k, p.y * k};
  return r;
}

int points(int k) {
  struct point a = {3, -4};
  struct point b = scale(a, k);
  return b.x * b.x + b.y * b.y;
}

int classify(int v) {
  switch (v) {
  case 0:
    return 10;
  case 1:
  case 2:
    return 20;
  case 7:
    return 70;
  case -3:
    return -30;
  default:
    return v * 2;
  }
}

int sort(int seed) {
  int values[10];
  unsigned state = (unsigned)seed;
  for (int i = 0; i < 10; ++i) {
    state = state * 1103515245u + 12345u;
    values[i] = (int)(state >> 16) % 100;
  }
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j + 1 < 10 - i; ++j)
      if (values[j] > values[j + 1]) {
        int t = values[j];
        values[j] = values[j + 1];
        values[j + 1] = t;
      }
  int checksum = 0;
  for (int i = 0; i < 10; ++i)
    checksum = checksum * 31 + values[i];
  return checksum;
}

int gcd(int a, int b) {
  while (b != 0) {
    int t = a % b;
    a = b;
    b = t;
  }
  return a < 0 ? -a : a;
}

int collatz(int n) {
  int steps = 0;
  unsigned v = (unsigned)n;
  while (v > 1) {
    v = (v & 1) ? 3 * v + 1 : v / 2;
    ++steps;
  }
  return steps;
}

static int table[16];

int histogram(int n) {
  for (int i = 0; i < 16; ++i)
    table[i] = 0;
  for (int i = 0; i < n; ++i)
    table[(i * 7) & 15] += i;
  int best = 0;
  for (int i = 1; i < 16; ++i)
    if (table[i] > table[best])
      best = i;
  return best * 1000 + table[best];
}
