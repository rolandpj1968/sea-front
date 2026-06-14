// EXPECT: 0
// An empty class has no observable storage, so a copy from any
// lvalue (`T y = expr;`) is a no-op. The canonical pathological
// case is `T y = *(T *)nullptr;` — gcc lazily elides the deref;
// sea-front would emit a real `(*x)` in C and segfault.
//
// Fix: when the var-decl's target type is an observably empty
// class (no data members, no virtual functions, all bases empty),
// drop the initializer entirely.
//
// Reduced from g++.dg/opt/empty1.C. N4659 §11.3.2 [dcl.ref] for
// "object representation"; class has none, so the copy reads
// nothing.

class empty_t {};

int main() {
    empty_t *x = 0;
    empty_t y = *x;  // must not actually deref *x
    (void)y;
    return 0;
}
