// EXPECT: 0
// `const T &r = f();` where f returns a class by value: the temp's
// lifetime extends through r's scope (N4659 §15.2/6 [class.temporary]).
// Sea-front lowers refs to pointers, so the binding becomes
// `T *r = &f();` — illegal C ('lvalue required as unary & operand').
//
// Hoist the call into a named local first so the ref binds to its
// address: `T __SF_temp_N = f(); T *r = &__SF_temp_N;`.
//
// Reduced from g++.dg/init/ref9.C — `const ex & tmpex = b.eval();`.

extern "C" void abort();

struct B {
    int n;
    B(int x) : n(x) {}
};

B make_b(int x) { return B(x); }

int main() {
    const B &r = make_b(42);
    if (r.n != 42) abort();
    return 0;
}
