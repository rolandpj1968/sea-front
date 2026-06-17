// EXPECT: 0
// N4659 §15.3/15 + §15.4 [except.handle]: when a dtor body is a
// function-try-block and the body throws, base subobjects must be
// destroyed BEFORE entering the catch handler — the handler exists
// to react to a failure during destruction, by which point the
// bases have necessarily been destroyed.
//
// Sea-front's catch-emit injects the base-dtor calls at the top of
// each handler when g_dtor_ftb_class is set (in emit_func_def /
// emit_method_as_free_fn for a dtor whose body is the
// function-try-block shape).
//
// Reduced from g++.dg/eh/dtor1.C.

extern "C" void abort(void);

int g_ad = 0;   /* A's dtor count */

struct A {
    ~A() { ++g_ad; }
};

struct B : A {
    ~B();
};

B::~B()
try {
    throw 1;
}
catch (...) {
    /* g_ad must already be 1 — A's dtor ran before this handler. */
    if (g_ad != 1) abort();
    return;
}

int main() {
    { B b; }
    return 0;
}
