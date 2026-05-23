// EXPECT: 0
// `new T[N]` and `new (p) T[N]()` must run T's default ctor on
// EACH element, not just the first. Pre-fix sea-front called the
// ctor only on element 0, leaving elements 1..N-1 with malloc'd
// bytes. With a placement-new poison-the-storage idiom, the
// elements after [0] read random poison bytes.
//
// The count expression must evaluate exactly once (its side
// effects must not double-fire). Sea-front captures it into
// `__sf_new_n` before emitting the malloc-call and the ctor loop;
// codegen_temp_name substitutes the captured local in both the
// malloc-call's `count * sizeof(T)` arg and the loop bound.
//
// Pattern: g++.dg/expr/anew4.C — placement array new with value-init.

#include <stdlib.h>
#include <string.h>

extern "C" void abort();

int call_count = 0;
unsigned get_n() { ++call_count; return 5; }

struct B {
    int n;
    B() { n = 137; }
};

struct D : public B {
    double x;
};

int main() {
    // Scalar new[N] with synth ctor: every element gets B's ctor.
    D* a = new D[get_n()];
    if (call_count != 1) return 1;
    for (int i = 0; i < 5; i++) if (a[i].n != 137) return 2;

    // Placement new[N]() with value-init + ctor loop: storage is
    // poisoned, value-init must zero it, then synth ctor sets n=137
    // for each element.
    void* p = malloc(5 * sizeof(D));
    memset(p, 0xFF, 5 * sizeof(D));
    D* b = new (p) D[5]();
    for (int i = 0; i < 5; i++) {
        if (b[i].n != 137) return 3;
        if (b[i].x != 0.0) return 4;
    }
    free(p);
    return 0;
}
