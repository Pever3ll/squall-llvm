/* Squall C library: assertions halt the program with the trap status. */
#undef assert
#ifdef NDEBUG
#define assert(condition) ((void)0)
#else
#define assert(condition) ((condition) ? (void)0 : __builtin_trap())
#endif

#if !defined(__cplusplus) && __STDC_VERSION__ >= 201112L &&                    \
    !defined(static_assert)
#define static_assert _Static_assert
#endif
