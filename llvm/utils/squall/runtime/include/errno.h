/* Squall C library: error numbers. */
#ifndef SQUALL_ERRNO_H
#define SQUALL_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#define EDOM 33
#define ERANGE 34
#define EINVAL 22

#ifdef __cplusplus
}
#endif

#endif
