// EXPECT: 0
// Local reference-typed variable used as the receiver of a member
// call. Sea-front lowers refs to pointers; ND_IDENT emission
// auto-derefs `aa` to `(*aa)`, which works for value-context reads
// but breaks the method-call lowering: `aa.f()` emitted as
// `(*aa)->__sf_vptr->f(aa)` tries to apply `->` to a struct value.
//
// The virtual-call codegen already suppresses the auto-deref for
// reference PARAMETERS (`is_ref_param`); this fix extends the same
// suppression to local ref vars via the resolved_decl->type check.
//
// Real-world hit: g++.dg/cpp0x/range-for15.C — `A &aa = b;` then
// `for (int x : aa)` where the range-for lowers to `aa.begin()`.

extern "C" void abort();

struct A {
    int x;
    A() : x(0) { }
    virtual int read() { return x; }
};

int main() {
    A a;
    a.x = 7;
    A &aa = a;
    if (aa.read() != 7) abort();
    return 0;
}
