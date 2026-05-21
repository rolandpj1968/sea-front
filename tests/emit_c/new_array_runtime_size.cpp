// EXPECT: 0
// `new T[N]` with a non-constant size expression must:
//   1. Evaluate the size expression exactly once (for its side
//      effects).
//   2. Allocate storage for N elements (not one).
// N4659 §8.3.4 [expr.new].
//
// Sea-front previously parsed the `[N]` bracket but dropped the
// size expression, lowering to `malloc(sizeof(T))` regardless.
// The size now propagates onto the parsed Type's array_size_expr,
// the lowering multiplies sizeof(T) by it, and the cast target
// becomes `T*` (per §8.3.4/4).
//
// Mirrors g++.dg/template/new1.C.

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);

int size_calls = 0;

unsigned get_n() { ++size_calls; return 4; }

struct A {
    static int ctor_calls;
    A() { ++ctor_calls; }
};
int A::ctor_calls = 0;

int main() {
    A *p = new A[get_n()];
    /* Sea-front doesn't yet ctor-loop array-new elements, but the
     * size expression must run for its side effect and the cast
     * target must be `T*`. */
    if (size_calls != 1) return 1;
    if (!p) return 2;
    free(p);
    return 0;
}
