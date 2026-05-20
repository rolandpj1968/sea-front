// EXPECT: 0
// Class temp hoist: when an inline `T()` expression hoists to
// __SF_temp_N, the synthesized default ctor must run if one exists.
// Without this fix, `B()` for `struct B { A a; };` where A's user
// ctor has side effects (++count) was lowered to `(struct B){0}` —
// the synthesized B-default-ctor (which would have chained into
// A's ctor) was skipped, so the side effects never fired.
//
// Pattern: g++.dg/init/elide2.C (which still needs copy-ctor-on-
// pass-by-value to fully pass, but the underlying synth-ctor gap
// is what this regression locks in).

int ctor_calls = 0;

struct A {
    A() { ++ctor_calls; }
};

struct B {
    A a;     // B has no user ctor; synthesized B::B() must chain
             // into A::A() — has_default_ctor=true on B.
};

int main() {
    // Force B() to land in expression position so the hoist path
    // runs. The result's address is taken to force materialization.
    const B *p = &(B());
    (void)p;
    // A's ctor should have fired exactly once.
    return ctor_calls == 1 ? 0 : 1;
}
