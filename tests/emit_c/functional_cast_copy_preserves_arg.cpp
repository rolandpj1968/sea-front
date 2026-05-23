// EXPECT: 0
// `T(x)` functional cast where T has no user copy ctor used to
// silently drop the arg and emit `T temp = {0}`. The arg's
// value vanished entirely — a silent-corruption trap.
//
// Now the copy falls through to bitwise (memberwise) copy via
// `T temp = x;` which preserves the value. Doesn't help when
// the class needs a non-trivial copy ctor invocation (e.g.
// invoking a template ctor — that's covered by the dg-xfail
// for implicit2.C), but at least the arg's data survives.

extern "C" void abort();

struct P { int x; int y; };

int main() {
    P a = { 7, 13 };

    // Functional-cast copy: should yield a P with the same fields.
    P b = P(a);
    if (b.x != 7 || b.y != 13) return 1;

    // In an expression position too — pre-fix this dropped a's
    // contents and the comparison used uninitialised bytes.
    if (P(a).x != 7 || P(a).y != 13) return 2;

    return 0;
}
