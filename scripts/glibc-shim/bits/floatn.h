/* sea-front shim: gcc 4.8 has no _FloatN types. Disable every
 * __HAVE_FLOAT* macro so math.h / floatn-common.h skip the
 * _Float16 / _Float32 / _Float64 / _Float128 / _Float32x / _Float64x /
 * _Float128x declarations and template specializations entirely.
 *
 * Tests that genuinely USE _FloatN arithmetic are out of scope for the
 * gcc 4.8 bootstrap. */
#ifndef _BITS_FLOATN_H
#define _BITS_FLOATN_H

#define __HAVE_FLOAT128 0
#define __HAVE_DISTINCT_FLOAT128 0
#define __HAVE_FLOAT64X 0
#define __HAVE_FLOAT64X_LONG_DOUBLE 0

#endif /* _BITS_FLOATN_H */
