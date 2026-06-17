// EXPECT: 0
// A function template invoked with two different cv-qualifications
// of the same underlying type must produce two distinct
// instantiations. The dedup key in template_instantiate (via
// type_to_key) didn't encode cv-qualifiers, so `qMin<Foo>` and
// `qMin<const Foo>` collapsed onto the first instantiation — only
// one def was emitted but both call sites referenced differently-
// mangled symbols → undefined reference.
//
// Reduced from g++.dg/expr/lval2.C.

enum Foo { A, B };

template<typename T>
T &qMin(T &a, T &b) { return a < b ? a : b; }

int main() {
    Foo f = A, g = B;
    Foo       &h = qMin(f, g);                          /* T = Foo */
    const Foo &i = qMin((const Foo&)f, (const Foo&)g);  /* T = const Foo */
    (void)h; (void)i;
    return 0;
}
