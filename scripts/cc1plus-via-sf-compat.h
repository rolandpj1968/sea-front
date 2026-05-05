/* cc1plus-via-sf-compat.h — neuter post-gcc-4.8 glibc decorators.
 *
 * Force-included AFTER the host's <sys/cdefs.h> so we override the
 * extended __malloc__(deallocator, argno) attribute form (gcc-11+)
 * with the empty form gcc 4.8's frontend can parse. Same trick for
 * the access decorators that didn't exist in 4.8.
 *
 * Used via 'g++ -include <this>' inside cc1plus-via-sf and the
 * g++.dg testsuite runners. */

#ifndef SEA_FRONT_CC1PLUS_VIA_SF_COMPAT
#define SEA_FRONT_CC1PLUS_VIA_SF_COMPAT 1

/* Pull in cdefs.h FIRST so its #defines run, then our undefs+redefs
 * below win. Without this the order would be: this header (overrides),
 * then cdefs.h (re-overrides to the broken form). */
#include <sys/cdefs.h>

#ifdef __attr_dealloc
#  undef __attr_dealloc
#endif
#define __attr_dealloc(dealloc, argno) /* gcc 4.8: not supported */

#ifdef __attr_dealloc_free
#  undef __attr_dealloc_free
#endif
#define __attr_dealloc_free /* gcc 4.8: not supported */

#ifdef __attr_dealloc_fclose
#  undef __attr_dealloc_fclose
#endif
#define __attr_dealloc_fclose /* gcc 4.8: not supported */

#ifdef __attribute_malloc__
/* glibc may have already expanded to __attribute__((__malloc__))
 * with a SINGLE arg — that gcc 4.8 supports; leave as-is. */
#endif

#ifdef __attr_access
#  undef __attr_access
#endif
#define __attr_access(x) /* gcc 4.8: not supported */

#ifdef __fortified_attr_access
#  undef __fortified_attr_access
#endif
#define __fortified_attr_access(a, t, s) /* gcc 4.8: not supported */

#ifdef __attribute_alloc_size__
#  undef __attribute_alloc_size__
#endif
#define __attribute_alloc_size__(...) /* gcc 4.8: not supported */

#ifdef __attribute_alloc_align__
#  undef __attribute_alloc_align__
#endif
#define __attribute_alloc_align__(x) /* gcc 4.8: not supported */

/* C23 _Float types don't exist in gcc 4.8's frontend. Map to nearest
 * existing type so declarations parse; tests that genuinely USE
 * _Float128 arithmetic are out of scope. */
#define _Float32  float
#define _Float64  double
#define _Float128 double
#define _Float32x float
#define _Float64x double

#endif /* SEA_FRONT_CC1PLUS_VIA_SF_COMPAT */
