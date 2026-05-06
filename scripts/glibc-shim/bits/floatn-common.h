/* sea-front shim: companion to floatn.h — disable every _FloatN type
 * for gcc 4.8 compatibility. */
#ifndef _BITS_FLOATN_COMMON_H
#define _BITS_FLOATN_COMMON_H

#include <bits/floatn.h>

#define __HAVE_FLOAT16 0
#define __HAVE_FLOAT32 0
#define __HAVE_FLOAT64 0
#define __HAVE_FLOAT32X 0
#define __HAVE_FLOAT128X 0
#define __HAVE_DISTINCT_FLOAT16 0
#define __HAVE_DISTINCT_FLOAT32 0
#define __HAVE_DISTINCT_FLOAT64 0
#define __HAVE_DISTINCT_FLOAT32X 0
#define __HAVE_DISTINCT_FLOAT64X 0
#define __HAVE_DISTINCT_FLOAT128X 0
#define __HAVE_FLOAT128_UNLIKE_LDBL 0
#define __HAVE_FLOATN_NOT_TYPEDEF 0

#endif /* _BITS_FLOATN_COMMON_H */
