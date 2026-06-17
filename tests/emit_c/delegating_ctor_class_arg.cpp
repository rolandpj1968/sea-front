// EXPECT: 0
// Delegating ctor with class-typed mem-init args:
//   `B(int i) : B(A(i)) {}` — the A temp from the functional cast
// must be ctor-constructed before the delegating call, and dtor-
// destroyed after. N4659 §15.2/3 [class.temporary]: temporaries
// die at the end of the full-expression. The delegating call IS
// one full-expression, so the temp's dtor runs immediately after.
//
// Reduced from g++.dg/cpp0x/dc6.C.

extern "C" void abort(void);

int a_ct = 0;

struct A {
    int i;
    A(int v) : i(v) { ++a_ct; }
    A(const A &a) : i(a.i) { ++a_ct; }
    ~A() { --a_ct; }
};

struct B {
    A a;
    B(A x) : a(x) {}              /* target ctor */
    B(int v) : B(A(v)) {}         /* delegating */
};

int main() {
    {
        B b(42);
        if (b.a.i != 42) abort();
    }
    /* All A ctors/dtors must balance — no leaked temp. */
    if (a_ct != 0) abort();
    return 0;
}
