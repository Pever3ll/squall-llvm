/* Squall C library: standard input/output. Squall has no I/O devices: a
   program reports results through its entry function's return value and
   memory. The functions are declared so that code which includes this
   header, or calls them only on debugging paths, compiles; a call that
   reaches the linked program is reported as having no definition. */
#ifndef SQUALL_STDIO_H
#define SQUALL_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)

int printf(const char *restrict format, ...);
int vprintf(const char *restrict format, va_list arguments);
int puts(const char *s);
int putchar(int c);

#ifdef __cplusplus
}
#endif

#endif
