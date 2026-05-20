// EXPECT: 0
// EH: 'throw' as a non-statement sub-expression — N4659 §8.17
// [expr.throw]/1: a throw-expression has type void but may appear
// as one arm of a ?: conditional (the result type is taken from
// the non-throw arm; control transfers via throw).
//
// Previous emit silently dropped throws in expression position with
// `0 /* throw */`. Pattern: g++.dg/eh/cond5.C / cond6.C —
//   `(x ? true : throw 1)` and `(x ? throw 2 : true)`.
//
// Fix: emit `({ __SF_THROW_PRIM(...); 0; })` (GNU stmt-expr) so the
// __SF_THROW_PRIM's goto-to-handler runs from the right control-flow
// position. The trailing `0` is unreachable but satisfies C's
// requirement that the stmt-expr yield a typed value.

extern "C" void abort();

int foo(bool x, int y) {
    if (y < 10 && (x ? true : throw 1)) y++;
    if (y > 20 || (x ? true : throw 2)) y++;
    return y;
}

int main() {
    // x=true: never throws, both branches run y++ once each.
    if (foo(true, 0) != 2) abort();
    // x=false, y=0: first cond enters throw 1.
    try { foo(false, 0); abort(); }
    catch (int i) { if (i != 1) abort(); }
    // x=false, y=10: first cond y<10 false → no throw; second cond
    // y>20 false → enters throw 2.
    try { foo(false, 10); abort(); }
    catch (int i) { if (i != 2) abort(); }
    return 0;
}
