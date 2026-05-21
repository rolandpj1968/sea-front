// EXPECT: 0
// `T v = T(args);` (copy-init from a ctor temp) must invoke T's ctor
// on v with elision — N4659 §11.6/14 [dcl.init]. The C-level `T v =
// (T){0};` is bitwise zero, skipping the ctor's side effects.
//
// Regression for the rewrite_copy_init_ctor_call helper (3776f08) +
// the synth default-ctor fall-back in the direct-init emit path
// (7015e4b). Three shapes exercised:
//   - Zero-arg default ctor with observable side effect.
//   - Multi-arg ctor with field-init side effect.
//   - Synthesised default ctor (class has virtual method → has_default_ctor
//     true but no user ctor; the fall-back must call sf__C__ctor).

int ctor_calls = 0;
int copy_ctor_calls = 0;

struct A {
    int x;
    A() : x(7) { ++ctor_calls; }
    A(int v) : x(v) { ++ctor_calls; }
    A(const A &o) : x(o.x) { ++copy_ctor_calls; }
};

struct WithVirtual {
    int y;
    WithVirtual() : y(42) { ++ctor_calls; }
    virtual void f() {}    // forces has_default_ctor=true (vptr install)
};

int main() {
    A a = A();
    if (a.x != 7) return 1;
    if (ctor_calls != 1) return 2;

    A b = A(99);
    if (b.x != 99) return 3;
    if (ctor_calls != 2) return 4;

    WithVirtual w = WithVirtual();
    if (w.y != 42) return 5;
    if (ctor_calls != 3) return 6;

    // Elision: copy ctor should NOT have fired despite `T v = T()`
    // syntax (the rewrite collapses to direct-init).
    if (copy_ctor_calls != 0) return 7;

    return 0;
}
