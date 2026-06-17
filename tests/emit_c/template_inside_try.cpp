// EXPECT: 0
// `C<int> x;` declared INSIDE a try-block (or any try/catch/throw
// subtree) still needs to trigger C's instantiation. Sea-front's
// instantiate.collect_from_node didn't visit ND_TRY / ND_HANDLER /
// ND_THROW children, so var-decls and template-id types there
// stayed un-instantiated — `struct sf__C_t_int_te_ x;` would
// reference an undefined struct, and a method call on x emitted as
// an implicit-decl warning + link error.
//
// Also clone.c didn't clone ND_TRY / ND_HANDLER / ND_THROW
// operands, so the cloned body of an instantiated method's throw
// statement lost its operand (emit produced empty `__SF_THROW_PRIM(,
// ...);`).
//
// Reduced from g++.dg/eh/template1.C.

extern "C" void abort(void);

template<typename T>
struct C {
    T v;
    void f() { throw v; }
};

int main() {
    try {
        C<int> x;
        x.v = 42;
        x.f();
    }
    catch (int i) {
        if (i != 42) abort();
        return 0;
    }
    abort();
}
