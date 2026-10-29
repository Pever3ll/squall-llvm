// Squall C library: string functions. Built with -fno-builtin so that these
// loops are not turned back into calls to themselves.

#include <string.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    while (n--)
      d[n] = s[n];
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = dst;
  while (n--)
    *d++ = (unsigned char)c;
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; --n, ++x, ++y)
    if (*x != *y)
      return *x - *y;
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (; n; --n, ++p)
    if (*p == (unsigned char)c)
      return (void *)p;
  return NULL;
}

size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n])
    ++n;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b)
    ++a, ++b;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; --n, ++a, ++b)
    if (*a != *b || !*a)
      return (unsigned char)*a - (unsigned char)*b;
  return 0;
}

char *strcpy(char *restrict dst, const char *restrict src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; ++i)
    dst[i] = src[i];
  for (; i < n; ++i)
    dst[i] = 0;
  return dst;
}

char *strcat(char *restrict dst, const char *restrict src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

char *strncat(char *restrict dst, const char *restrict src, size_t n) {
  char *d = dst + strlen(dst);
  while (n-- && *src)
    *d++ = *src++;
  *d = 0;
  return dst;
}

char *strchr(const char *s, int c) {
  for (;; ++s) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return NULL;
  }
}

char *strrchr(const char *s, int c) {
  const char *found = NULL;
  for (;; ++s) {
    if (*s == (char)c)
      found = s;
    if (!*s)
      return (char *)found;
  }
}

char *strstr(const char *haystack, const char *needle) {
  size_t n = strlen(needle);
  for (; *haystack; ++haystack)
    if (!strncmp(haystack, needle, n))
      return (char *)haystack;
  return n ? NULL : (char *)haystack;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  while (s[n] && strchr(accept, s[n]))
    ++n;
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  while (s[n] && !strchr(reject, s[n]))
    ++n;
  return n;
}

char *strpbrk(const char *s, const char *accept) {
  s += strcspn(s, accept);
  return *s ? (char *)s : NULL;
}
