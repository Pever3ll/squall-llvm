static int bias = 3;

static int twice(int x) { return x * 2; }

int kernel(int n) {
  int total = 0;
  for (int i = 0; i < n; ++i)
    total += twice(i);
  return total + bias;
}
