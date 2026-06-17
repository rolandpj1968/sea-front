// EXPECT: 0
// Dtor function-try-block where the body does NOT throw — the
// try-body completes normally, no catch handler runs. Per N4659
// §15.4 base destruction still happens; sea-front injects base
// dtors at the try's normal-exit (mirror of the catch-handler
// injection) AND the wrapper D1 SKIPS its base-dtor calls so
// destruction runs exactly once.
//
// Before the wrapper-skip fix, A's dtor ran TWICE — once at the
// try-body normal-exit injection, once from D1.

extern "C" void abort(void);

int g_ad = 0;

struct A {
    ~A() { ++g_ad; }
};

struct B : A {
    ~B();
};

B::~B()
try {
    /* No throw — body completes normally. */
}
catch (...) {
    abort();  /* must not reach */
}

int main() {
    { B b; }
    if (g_ad != 1) abort();  /* exactly one A destruction */
    return 0;
}
