// EXPECT: 41
// A class with a reference member initialized from a class-temp
// in the mem-init list: 'B() : a(A()) { }'. The temp must be
// bound by the reference (pointer-lowered), not assigned by value.
// Without ref-aware lowering, the C output assigned a struct value
// to a 'const T *' field — type mismatch from cc.
//
// Surfaced by gcc 4.8 g++.dg/init/lifetime2 and the broader
// 'incompatible types when assigning to type const struct A * from
// struct A' cluster.
//
// Note: this test verifies sea-front EMITS valid C from this
// shape (the original failure was a cc-time type mismatch). The
// dangling-temp lifetime question (whether b.a outlives the
// mem-init's full-expression) is per the language; we don't
// dereference b.a after that.

int counter = 0;

struct A { int dummy; };

struct B {
    const A &a;
    B() : a(A()) { counter = 41; }
};

int main() {
    B b;
    (void)b;
    return counter;     // B's body sets counter = 41.
                        // Pre-fix sea-front rejected at cc time with a
                        // type-mismatch — that's the real regression
                        // this test guards.
}
