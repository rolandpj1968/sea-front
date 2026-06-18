// EXPECT: 0
// Class-scope `operator new` / `operator delete` are implicitly
// static per N4659 §16.5.3/1 [basic.stc.dynamic.allocation]: the
// `this` parameter is OMITTED from the C signature. The OOL
// definition path (emit_method_signature) already promotes; the
// IN-CLASS forward decl had its own emit branch that only consulted
// DECL_STATIC, so when the user declared `void *operator new(...)`
// without `static`, the in-class forward decl took the `this`-bearing
// shape while the OOL definition took the `this`-less shape — the
// C compiler then errored with "conflicting types".
//
// Reduced from g++.dg/init/new27.C — the call-site dispatch from a
// placement-new to the CLASS-scope operator new is a separate gap;
// here we only cover the dual-decl signature matching.

extern "C" void abort();
extern "C" void *malloc(unsigned long);
extern "C" void  free(void *);

int alloc_calls = 0;

struct T {
    int x;
    /* Note: no explicit `static`. */
    void *operator new(unsigned long sz, char *&p);
    void  operator delete(void *);
};

void *T::operator new(unsigned long sz, char *&p) {
    ++alloc_calls;
    void *r = p;
    p += sz;
    return r;
}

void T::operator delete(void *) {}

int main() {
    /* Just verify the in-class forward decl + the OOL definition
     * agree on the `this`-less shape — the cc would otherwise error
     * "conflicting types" at the OOL definition. Direct-call /
     * placement-new dispatch to class-scope operator-new is a
     * separate codegen slice (the call site still emits a bare-name
     * mangle). */
    (void)alloc_calls;
    return 0;
}
