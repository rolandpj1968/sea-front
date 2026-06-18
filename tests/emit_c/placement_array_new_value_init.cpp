// EXPECT: 0
// `new (p) T[n]()` with a runtime n needs:
//   (a) the alloc size to multiply by n (was emitting `sizeof(T[])`,
//       which cc rejects as incomplete type — parse_type_name's
//       abstract array suffix was dropping non-literal extents);
//   (b) value-init to zero ALL n elements (was emitting `*p = 0`,
//       which only zeroed the first slot).
//
// parse_type_name now captures the bracket expression onto
// `array_size_expr`; the new-expression's scalar value-init path
// emits memset over the full allocation when new_array_count is set.
// N4659 §8.3.4/15 + §8.5/8 [expr.new / dcl.init].
//
// Reduced from g++.dg/expr/anew1.C.

extern "C" void abort();
extern "C" void *malloc(unsigned long);
extern "C" void *memset(void *, int, unsigned long);

int *allocate(int n) {
    void *p = malloc(n * sizeof(int));
    memset(p, 0xff, n * sizeof(int));  /* dirty so value-init must overwrite */
    return new (p) int[n]();
}

int main() {
    int n = 17;
    int *p = allocate(n);
    for (int i = 0; i < n; ++i)
        if (p[i] != 0) abort();
    return 0;
}
