// EXPECT: 0
// Direct-list-init 'T x{a, b}' mandates left-to-right initializer
// evaluation per N4659 §11.6.4 [dcl.init.list]/4. Plain C function-
// call args are unsequenced (C11 §6.5.2.2/10) so the naive lowering
// 'ctor(&x, (i++), (i++))' would let either ++ run first; on x86-64
// gcc the right arg wins and we'd get i=1 / j=0, not i=0 / j=1.
// Sea-front detects side-effect args and hoists each into a
// sequenced temp before the ctor call.
//
// Pattern: g++.dg/cpp0x/initlist86.C.

struct A {
    int i, j;
    A(int a, int b) : i(a), j(b) {}
};

int main() {
    int i = 0;
    A a{i++, i++};
    if (a.i != 0 || a.j != 1) return 1;

    // Paren-init has the same per-arg sequencing requirement
    // (each arg fully evaluated before another) per N4659 §8.2.2
    // [expr.call]/5 — sea-front applies the same hoist.
    int k = 10;
    A b(k++, k++);
    if (b.i != 10 || b.j != 11) return 2;

    // Mix: side-effecting + pure args. Only the side-effecting arg
    // gets hoisted; the literal stays inline.
    int m = 100;
    A c{m++, 200};
    if (c.i != 100 || c.j != 200) return 3;

    return 0;
}
