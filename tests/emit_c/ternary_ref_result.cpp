// EXPECT: 0
// `cond ? ref_a : ref_b` is itself a glvalue of the ref type (N4659
// §8.16/4 [expr.cond]: both arms glvalues of the same type → result
// is a glvalue). In sea-front T& is lowered to T* at C level, so a
// ref-param/ref-local read normally emits `(*p)` to recover the
// referent. When the whole ternary is reference-typed, its caller
// expects the lowered T* pointer form (e.g. a function returning T&
// emits `return ternary;` and the C signature is `T* fn(...)`).
//
// Sea-front used to emit `cond ? (*a) : (*b)` — value of the chosen
// referent — and the enclosing `return` slot received an int where
// a `T*` was expected. cc lets that through as int-to-pointer with
// a warning, but the resulting pointer is garbage and any deref
// segfaults.
//
// The ternary emit now suppresses ref-deref on its arms when its own
// resolved_type is TY_REF/TY_RVALREF, keeping the underlying lowered
// pointer through the ?:.
//
// Reduced from g++.dg/expr/lval2.C; nrv13.C exercises the nested
// ?:-of-refs shape via the same mechanism.

extern "C" void abort();

template<typename T>
T &qMin(T &a, T &b) {
    return a < b ? a : b;
}

int main() {
    int x = 1, y = 2;
    int &h = qMin(x, y);
    if (&h != &x) abort();

    int p = 5, q = 3;
    int &k = qMin(p, q);
    if (&k != &q) abort();

    /* const-ref variant — same code path with cv on the pointer. */
    const int cx = 7, cy = 4;
    const int &ck = qMin<const int>(cx, cy);
    if (&ck != &cy) abort();

    return 0;
}
