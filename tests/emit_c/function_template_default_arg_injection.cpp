// EXPECT: 7
// Function template with a trailing default argument; called with
// fewer args than params. The instantiation pass produces a TY_FUNC
// for the cloned function — that TY_FUNC must carry param_defaults
// so emit_call can inject the default at the call site.
//
// Without param_defaults on the synthesized TY_FUNC, the call
// `reserve(p, 1)` emits `reserve_t_int_te_(p, 1)` against a
// 3-param signature → "too few arguments" error.
//
// Pattern: gcc 4.8 vec.h `vec_safe_reserve(v, n, exact = false)`
// called as `vec_safe_reserve(die->die_attr, 1)`.

template<typename T>
int reserve(T *p, int nelems, bool exact = false) {
    return *p + nelems + (exact ? 100 : 0);
}

int main() {
    int x = 4;
    return reserve(&x, 3);   // exact defaults to false; 4 + 3 = 7
}
