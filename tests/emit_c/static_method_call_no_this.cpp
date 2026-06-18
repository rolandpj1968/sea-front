// EXPECT: 0
// `obj.f()` / `p->f()` where f is a static member must dispatch
// with NO `this` argument — N4659 §11.4.9.1 [class.static.mfct]/2:
//   "A static member function does not have a this pointer."
// The qualified-id form `Class::f()` already routed correctly
// (no receiver to drop). The unqualified-receiver shape was
// emitting `&obj`/obj as the first arg, so cc rejected with
// "too many arguments to function".
//
// Reduced from g++.dg/init/lifetime3.C `pf->foo()`.

extern "C" void abort();

struct B {
    static int answer() { return 42; }
};

B g_b;
B *get_b() { return &g_b; }

int main() {
    B  b;
    B *p = &b;
    if (b.answer()    != 42) abort();   /* object-expression, static dispatch */
    if (p->answer()   != 42) abort();   /* pointer-expression, static dispatch */
    if (get_b()->answer() != 42) abort();
    if (B::answer()   != 42) abort();   /* qualified-id form */
    return 0;
}
