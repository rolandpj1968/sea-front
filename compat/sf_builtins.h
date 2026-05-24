/* sf_builtins.h — portable C99 implementations of the GCC __builtin_*
 * primitives sea-front passes through from C++ source. Intended for
 * back-ends that don't natively implement them (cproc). gcc/clang
 * already provide these as compiler builtins so the explicit
 * definitions here are harmless under those compilers — they'll be
 * shadowed by the builtin / inlined.
 *
 * Wired via scripts/cproc++ which adds '-include compat/sf_builtins.h'
 * to cproc's preprocess step. Don't include this file directly from
 * sea-front-generated C — it's a back-end shim, not a runtime.
 *
 * Naming: 'sf_' prefix on the header / location, but the symbols
 * themselves are the standard '__builtin_*' names so source code
 * doesn't need to know. Static-inline so each TU gets a private copy
 * (no link conflict, dead-code-elimable when unused).
 */

#ifndef SF_BUILTINS_H
#define SF_BUILTINS_H

/* memset/memcpy/memmove — gcc emits __builtin_* in some optimization
 * paths even at -O0. Delegate to libc; same as the gcc lowering when
 * the inliner doesn't fire. */
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memmove(void *, const void *, unsigned long);
static inline void *__builtin_memset(void *d, int c, unsigned long n) {
    return memset(d, c, n);
}
static inline void *__builtin_memcpy(void *d, const void *s, unsigned long n) {
    return memcpy(d, s, n);
}
static inline void *__builtin_memmove(void *d, const void *s, unsigned long n) {
    return memmove(d, s, n);
}

/* Count trailing zeros — bit-twiddling. For zero argument GCC says
 * the result is undefined (we conservatively return the type width).
 * The de-Bruijn sequence variant is faster on hardware without a
 * native ctz instruction; iterate-while is good enough at -O0. */
static inline int __builtin_ctz(unsigned int x) {
    if (x == 0) return 32;
    int n = 0;
    while ((x & 1u) == 0) { x >>= 1; ++n; }
    return n;
}
static inline int __builtin_ctzl(unsigned long x) {
    if (x == 0) return (int)(sizeof(unsigned long) * 8);
    int n = 0;
    while ((x & 1ul) == 0) { x >>= 1; ++n; }
    return n;
}
static inline int __builtin_ctzll(unsigned long long x) {
    if (x == 0) return 64;
    int n = 0;
    while ((x & 1ull) == 0) { x >>= 1; ++n; }
    return n;
}

/* Count leading zeros — scan from the top bit down. */
static inline int __builtin_clz(unsigned int x) {
    if (x == 0) return 32;
    int n = 0;
    while ((x & 0x80000000u) == 0) { x <<= 1; ++n; }
    return n;
}
static inline int __builtin_clzl(unsigned long x) {
    int w = (int)(sizeof(unsigned long) * 8);
    if (x == 0) return w;
    unsigned long mask = 1ul << (w - 1);
    int n = 0;
    while ((x & mask) == 0) { x <<= 1; ++n; }
    return n;
}
static inline int __builtin_clzll(unsigned long long x) {
    if (x == 0) return 64;
    int n = 0;
    while ((x & (1ull << 63)) == 0) { x <<= 1; ++n; }
    return n;
}

/* Population count — Brian Kernighan's bit-clear trick. */
static inline int __builtin_popcount(unsigned int x) {
    int n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
}
static inline int __builtin_popcountl(unsigned long x) {
    int n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
}
static inline int __builtin_popcountll(unsigned long long x) {
    int n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
}

/* Find-first-set: 1-based index of the LSB, 0 if x is zero. POSIX
 * ffs/ffsl/ffsll, also GCC built-ins. gcc 4.8's hwint.h uses ffsl. */
static inline int __builtin_ffs(int x) {
    if (x == 0) return 0;
    unsigned int u = (unsigned int)x;
    int n = 1;
    while ((u & 1u) == 0) { u >>= 1; ++n; }
    return n;
}
static inline int __builtin_ffsl(long x) {
    if (x == 0) return 0;
    unsigned long u = (unsigned long)x;
    int n = 1;
    while ((u & 1ul) == 0) { u >>= 1; ++n; }
    return n;
}
static inline int __builtin_ffsll(long long x) {
    if (x == 0) return 0;
    unsigned long long u = (unsigned long long)x;
    int n = 1;
    while ((u & 1ull) == 0) { u >>= 1; ++n; }
    return n;
}

/* Integer abs — libstdc++'s <bits/std_abs.h> uses these. */
static inline int __builtin_abs(int x) { return x < 0 ? -x : x; }
static inline long __builtin_labs(long x) { return x < 0 ? -x : x; }
static inline long long __builtin_llabs(long long x) { return x < 0 ? -x : x; }

/* Float abs — libstdc++'s <bits/std_abs.h> uses these for std::abs
 * overloads on float/double/long double. The 'l' variant uses
 * 'double' as the parameter type when SEA_LONG_DOUBLE_AS_DOUBLE
 * downgrade is in effect (sea-front emits long double as double in
 * that mode) — otherwise the back-end would have errored before
 * this header was ever consulted. */
static inline float __builtin_fabsf(float x) { return x < 0 ? -x : x; }
static inline double __builtin_fabs(double x) { return x < 0 ? -x : x; }
static inline double __builtin_fabsl(double x) { return x < 0 ? -x : x; }

/* Byte-swap. glibc's <bits/byteswap.h> uses these for ntoh/htons. */
static inline unsigned short __builtin_bswap16(unsigned short x) {
    return (unsigned short)((x << 8) | (x >> 8));
}
static inline unsigned int __builtin_bswap32(unsigned int x) {
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8)  | ((x & 0xff000000u) >> 24);
}
static inline unsigned long long __builtin_bswap64(unsigned long long x) {
    return ((x & 0x00000000000000ffull) << 56) |
           ((x & 0x000000000000ff00ull) << 40) |
           ((x & 0x0000000000ff0000ull) << 24) |
           ((x & 0x00000000ff000000ull) << 8)  |
           ((x & 0x000000ff00000000ull) >> 8)  |
           ((x & 0x0000ff0000000000ull) >> 24) |
           ((x & 0x00ff000000000000ull) >> 40) |
           ((x & 0xff00000000000000ull) >> 56);
}

/* __builtin_expect / __builtin_unreachable: cproc has built-in
 * support (see scope.c), so this shim must NOT redeclare them or
 * cproc errors with "redeclared with different kind". Left to the
 * compiler. gcc/clang likewise have them as builtins.
 *
 * __builtin_trap: not a cproc builtin; provide a portable fallback. */
extern void abort(void) __attribute__((__noreturn__));
static inline _Noreturn void __builtin_trap(void) { abort(); }

/* __cxa_thread_atexit — Itanium C++ ABI hook for TLS-object dtors.
 * libsupc++ / libstdc++ provides this when linked. Bare-C targets
 * (no libstdc++) need a stub — register the dtor in a TU-local list
 * and run on normal exit. This minimal version supports a small
 * fixed pool; real glibc handles arbitrary counts via a linked list.
 * Sufficient for sea-front's emitted TLS-class-dtor pattern in test
 * cases; for real applications, link a proper libstdc++.
 */
#ifndef SF_BUILTINS_NO_THREAD_ATEXIT
extern int atexit(void (*)(void));
struct __sf_tls_dtor { void (*f)(void *); void *obj; };
enum { __SF_TLS_DTOR_MAX = 64 };
static struct __sf_tls_dtor __sf_tls_dtors[__SF_TLS_DTOR_MAX];
static int __sf_tls_n_dtors = 0;
static void __sf_tls_run_dtors(void) {
    for (int i = __sf_tls_n_dtors - 1; i >= 0; --i)
        __sf_tls_dtors[i].f(__sf_tls_dtors[i].obj);
}
static inline int __cxa_thread_atexit(void (*f)(void *), void *obj, void *dso) {
    (void)dso;
    if (__sf_tls_n_dtors >= __SF_TLS_DTOR_MAX) return -1;
    if (__sf_tls_n_dtors == 0) atexit(__sf_tls_run_dtors);
    __sf_tls_dtors[__sf_tls_n_dtors].f = f;
    __sf_tls_dtors[__sf_tls_n_dtors].obj = obj;
    ++__sf_tls_n_dtors;
    return 0;
}
/* __dso_handle — opaque DSO identifier the Itanium ABI threads
 * through __cxa_thread_atexit. The system's crtbegin.o supplies a
 * strong definition when present; our weak fallback handles the
 * bare-C case (no crtbegin linked). Strong-beats-weak picks the
 * crtbegin one when both are present, so no clash. */
__attribute__((weak)) void *__dso_handle = (void *)0;
#endif /* SF_BUILTINS_NO_THREAD_ATEXIT */

#endif /* SF_BUILTINS_H */
