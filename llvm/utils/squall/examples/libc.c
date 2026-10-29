// Programs that use the Squall C library. They are linked (not compiled with
// -c), so the driver includes the runtime, and each is compared with the same
// source built against the host's C library.

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Builds a string of n letters and measures it several ways.
int strings(int n) {
  char buffer[64], copy[64];
  n = abs(n) % 40;
  for (int i = 0; i < n; ++i)
    buffer[i] = (char)('a' + i % 26);
  buffer[n] = 0;
  strcpy(copy, buffer);
  strncat(copy, "XYZ", 2);
  int result = (int)strlen(buffer) * 1000 + (int)strlen(copy);
  result += strcmp(buffer, copy) < 0 ? 7 : 0;
  result += strncmp(buffer, copy, (size_t)n) == 0 ? 11 : 0;
  result += memcmp(buffer, "abc", 3) == 0 ? 13 : 0;
  const char *z = strchr(copy, 'X');
  result += z ? (int)(z - copy) * 17 : -1;
  return result;
}

// Searching with strstr, strrchr, strspn, strcspn, and strpbrk.
int search(int which) {
  static const char *const texts[] = {
      "the quick brown fox", "a,b;;c", "   leading", "", "aaaaab", "no match"};
  const char *text = texts[(unsigned)which % 6];
  int result = 0;
  const char *found = strstr(text, "ab");
  result += found ? (int)(found - text) + 1 : 0;
  found = strstr(text, "quick");
  result = result * 31 + (found ? (int)(found - text) + 1 : 0);
  found = strrchr(text, 'a');
  result = result * 31 + (found ? (int)(found - text) + 1 : 0);
  result = result * 31 + (int)strspn(text, " a");
  result = result * 31 + (int)strcspn(text, ",;");
  found = strpbrk(text, ",;x");
  result = result * 31 + (found ? (int)(found - text) + 1 : 0);
  return result;
}

// Parsing in several bases, with the stopping point. long is 32 bits on
// Squall and usually 64 on the host, so its inputs fit in 32 bits; the long
// long conversions also see values that saturate.
int parse(int which) {
  static const char *const inputs[] = {
      "  -1234xyz",  "0x7fFF", "0755",  "+42", "zz",
      "-2147483648", "  12  ", "-0x10", "",    "2147483647"};
  static const char *const wide[] = {
      "99999999999",          "18446744073709551615",  "-9223372036854775809",
      "  0x1234567890abcdef", "777777777777777777777", "-1"};
  const char *s = inputs[(unsigned)which % 10];
  const char *w = wide[(unsigned)which % 6];
  char *end;
  int a = (int)strtol(s, &end, 0);
  int used = (int)(end - s);
  unsigned b = (unsigned)strtoul(s, NULL, 10);
  long long c = strtoll(w, &end, 0);
  int wide_used = (int)(end - w);
  unsigned long long d = strtoull(w, NULL, 8);
  unsigned long long e = strtoull(s, NULL, 36);
  return (int)((unsigned)a * 3u + (unsigned)used * 101u + (b & 0xffffu) +
               (unsigned)(c ^ c >> 32) + (unsigned)wide_used * 7u +
               (unsigned)(d ^ d >> 32) + (unsigned)(e & 0xffu) +
               (unsigned)atoi(s));
}

static int ascending(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

// Sorting distinct keys, then finding them.
int sorting(int seed) {
  int values[40];
  unsigned s = (unsigned)seed;
  for (int i = 0; i < 40; ++i) {
    s = s * 1103515245u + 12345u;
    values[i] = (int)(s >> 8) * 64 + i; // Distinct.
  }
  qsort(values, 40, sizeof values[0], ascending);
  int result = 0;
  for (int i = 0; i < 40; ++i)
    result = result * 7 + values[i];
  for (int i = 0; i < 40; i += 3) {
    int *at = bsearch(&values[i], values, 40, sizeof values[0], ascending);
    result += at ? (int)(at - values) : -1000;
  }
  int missing = values[5] + 1;
  result += bsearch(&missing, values, 40, sizeof values[0], ascending) ? 1 : 0;
  return result;
}

// Character classes over the ASCII range.
int classes(int c) {
  c = (unsigned)c % 128;
  // The classes return any nonzero value for true.
  return !!isalnum(c) | !!isalpha(c) << 1 | !!isblank(c) << 2 |
         !!iscntrl(c) << 3 | !!isdigit(c) << 4 | !!isgraph(c) << 5 |
         !!islower(c) << 6 | !!isprint(c) << 7 | !!ispunct(c) << 8 |
         !!isspace(c) << 9 | !!isupper(c) << 10 | !!isxdigit(c) << 11 |
         tolower(c) << 12 | toupper(c) << 20;
}

// Division helpers and memory functions on overlapping ranges.
int arithmetic(int n) {
  div_t q = div(n, 7);
  ldiv_t r = ldiv(n, -3);
  lldiv_t s = lldiv((long long)n * 100003, 97);
  char bytes[24];
  memset(bytes, 'a', sizeof bytes);
  memcpy(bytes, "0123456789", 10);
  memmove(bytes + 3, bytes, 10);
  memmove(bytes, bytes + 5, 8);
  const char *seven = memchr(bytes, '7', sizeof bytes);
  return q.quot * 13 + q.rem + r.quot * 17 + r.rem + (int)s.quot + (int)s.rem +
         bytes[0] + bytes[9] * 3 + (seven ? (int)(seven - bytes) : 99) +
         (int)labs(n) + (int)llabs(-(long long)n * 3);
}

// assert halts the program with the trap status when the condition fails.
int checked(int n) {
  assert(n >= 0);
  return n * 2;
}
