// EXPECT: 0
// Template ctors at the three principal call shapes:
//
//   1. Functional cast      `T x = T(5);`           (ND_CALL.is_type_call)
//   2. Direct-init var-decl `T x(5);`               (ND_VAR_DECL.has_ctor_init)
//   3. New-expression       `T *p = new T(5);`      (ND_CAST.is_new_expr)
//
// Pre-fix sea-front:
//   - 1 fell back to bitwise / aggregate init.
//   - 2 emitted `{5}` aggregate init (no ctor body ran).
//   - 3 either emitted `*p = 5` (invalid) or allocated without ctor call.
//
// Post-fix: sema's stamp_ctor_winner runs template-aware overload res
// at each site, builds an ND_TEMPLATE_ID on resolved_ctor_tid, the
// instantiation walker drives the member-template path to clone the
// ctor with class context, and emit calls the cloned func.
// N4659 §16.3 [over.match] + §15.1 [class.ctor] + §17.8.2 [temp.deduct].

extern "C" void abort();

struct A {
    int x;
    template <class T> A(T t) : x((int)t * 3) {}
};

int main() {
    // Shape 1: functional cast
    A a1 = A(5);
    if (a1.x != 15) abort();

    // Shape 2: direct-init var-decl
    A a2(7);
    if (a2.x != 21) abort();

    // Shape 3: new-expression
    A *a3 = new A(11);
    if (a3->x != 33) abort();

    return 0;
}
