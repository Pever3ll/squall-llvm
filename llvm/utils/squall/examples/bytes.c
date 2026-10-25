// Byte and 16-bit memory: strings, packed and padded structures, and
// unaligned accesses, through static and dynamic pointers.

typedef unsigned long size_t;

static const char greeting[] = "Hello, Squall!";
static const char *const words[] = {"alpha", "beta", "gamma", "delta"};
static char buffer[48];
static short samples[7] = {1, -2, 300, -32768, 32767, 5, -6};

struct record {
  char tag;
  short count;
  int value;
  char name[6];
};
static struct record records[3] = {
    {'a', 1, 100, "one"}, {'b', -2, -200, "two"}, {'c', 300, 3000, "three"}};

struct __attribute__((packed)) packed {
  char c;
  int i;
  short s;
};
static struct packed packs[2] = {{1, 0x12345678, -3}, {2, -5, 1000}};

static size_t length(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}

static void copy(char *d, const char *s) {
  while ((*d++ = *s++))
    ;
}

static int compare(const char *a, const char *b) {
  while (*a && *a == *b)
    ++a, ++b;
  return (unsigned char)*a - (unsigned char)*b;
}

static unsigned hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; ++s)
    h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

int string_length(int skip) { return (int)length(greeting + (skip & 7)); }

int reverse(int n) {
  int len = (int)length(greeting);
  for (int i = 0; i < len; ++i)
    buffer[i] = greeting[len - 1 - i];
  buffer[len] = 0;
  int sum = 0;
  for (int i = 0; i < len; ++i)
    sum = sum * 31 + buffer[i] * (i + n);
  return sum;
}

int concatenate(int which) {
  copy(buffer, words[which & 3]);
  copy(buffer + length(buffer), "-");
  copy(buffer + length(buffer), words[(which + 1) & 3]);
  return (int)(hash(buffer) ^ length(buffer));
}

int sort_letters(int seed) {
  char text[24];
  copy(text, words[seed & 3]);
  copy(text + length(text), words[(seed >> 2) & 3]);
  int n = (int)length(text);
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && text[j - 1] > text[j]; --j) {
      char t = text[j];
      text[j] = text[j - 1];
      text[j - 1] = t;
    }
  return (int)hash(text) + compare(text, words[seed & 3]);
}

int signed_bytes(int x) {
  signed char c[9];
  unsigned char u[9];
  for (int i = 0; i < 9; ++i) {
    c[i] = (signed char)(x + i * 37);
    u[i] = (unsigned char)(x - i * 53);
  }
  int s = 0;
  for (int i = 0; i < 9; ++i)
    s += c[i] * 3 + u[i];
  return s;
}

int shorts(int k) {
  int s = 0;
  for (int i = 0; i < 7; ++i) {
    samples[i] = (short)(samples[i] * k + i);
    s += samples[i];
  }
  return s;
}

static int describe(struct record r) {
  return r.tag * 1000 + r.count * 10 + r.value + r.name[0] + r.name[1];
}

int records_by_value(int i) {
  struct record r = records[i % 3];
  r.count = (short)(r.count + i);
  r.name[1] = 'X';
  records[(i + 1) % 3].tag = r.name[1];
  return describe(r) + records[(i + 1) % 3].tag;
}

static int sum_packed(struct packed p) { return p.c + p.i + p.s; }

int packed_fields(int x) {
  packs[1].i += x;
  packs[0].s = (short)(packs[0].s - 3 * x);
  return sum_packed(packs[0]) * 7 + sum_packed(packs[1]);
}

int adjacent_bytes(int x) {
  // Neighboring bytes of one cell, written through a pointer the compiler
  // cannot see through.
  char cells[8] = {0};
  char *volatile p = cells;
  for (int i = 0; i < 8; ++i)
    p[i] = (char)(x * (i + 1));
  p[3] += p[2];
  p[4] = p[7] - p[0];
  int s = 0;
  for (int i = 0; i < 8; ++i)
    s = s * 257 + (unsigned char)cells[i];
  return s;
}

int variable_copies(int n) {
  char src[40], dst[40];
  for (int i = 0; i < 40; ++i) {
    src[i] = (char)(i * 7 + n);
    dst[i] = 0;
  }
  int count = n % 30;
  __builtin_memcpy(dst + 1, src + 3, count);
  __builtin_memmove(src + 2, src, count / 2);
  __builtin_memset(dst + 30, n, n % 9);
  int s = 0;
  for (int i = 0; i < 40; ++i)
    s = s * 3 + dst[i] + src[i];
  return s;
}

int wide_division(int a, int b) {
  long long x = (long long)a * 1000003LL, y = b ? b : 7;
  unsigned long long u = (unsigned long long)x;
  return (int)(x / y) ^ (int)(x % y) ^
         (int)(u / (unsigned long long)(y * y + 1)) ^ (int)(u % 97);
}
