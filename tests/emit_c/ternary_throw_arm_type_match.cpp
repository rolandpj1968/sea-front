// EXPECT: 0
// `cond ? throw expr : <other>` (or with the arms swapped) — the
// throw arm is a void expression in C++ per N4659 §8.16/6 [expr.cond],
// and the common type is the other arm's type. Sea-front lowers
// throw at expression position as `({ __SF_THROW_*(...); 0; })`
// — the `0` is hardcoded as int, so when the other arm has a
// different type cc rejects with "type mismatch in conditional
// expression".
//
// Steer the throw's stmt-expr yield to the other arm's type
// via the g_throw_yield_ty global. The yield is never executed
// (the macro's `goto` jumps to the handler), so any value of the
// right type works — `(T){0}` is the simplest universal lowering.
//
// Reduced from g++.dg/eh/cond1.C.

extern "C" void abort();

struct has_destructor { ~has_destructor() {} };
struct no_destructor {};

int caught = 0;

template<class T> void run(T (*body)(int), int n) {
    try { (void)body(n); abort(); } catch (int) { ++caught; }
}

has_destructor c1(int x) { return (x ? throw 0 : has_destructor()); }
no_destructor  c2(int x) { return (x ? throw 0 : no_destructor());  }
has_destructor c3(int x) { return (x ? has_destructor() : throw 0); } /* will not throw with x=1 */
int            c4(int x) { return (x ? throw 0 : 5);                } /* regression guard: arm-type-matches */

int main(int argc, char **) {
    /* throw arm exercised (cond=true), other arm class type */
    try { (void)c1(argc+1); abort(); } catch (int) { ++caught; }
    try { (void)c2(argc+1); abort(); } catch (int) { ++caught; }

    /* throw arm NOT exercised — statically-false cond, throw still
     * lowered & type-matched */
    (void)c3(1);
    if (caught != 2) abort();

    /* int-vs-int already worked — keep as regression guard */
    try { (void)c4(1); abort(); } catch (int) { ++caught; }
    if (caught != 3) abort();

    return 0;
}
