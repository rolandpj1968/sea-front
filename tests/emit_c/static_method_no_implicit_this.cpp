// EXPECT: 0
// N4659 §11.4.9 [class.static] — static member functions take no
// implicit 'this'. An unqualified call from one static method to
// another sibling static method must not pass 'this' as the first arg.
//
// Previously broken: emit_c.c's implicit-this lowering for unqualified
// same-class calls always emitted `f(this, args...)`. Surfaced when
// the OOL member-template instantiation began emitting bodies that
// contain such calls (gcc 4.8 vec.h va_heap::reserve calling release).
struct A {
    static int inner(int x) { return x + 1; }
    static int outer(int x) { return inner(x); }
};
int main() { return A::outer(-1); }
