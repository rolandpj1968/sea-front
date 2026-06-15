// EXPECT: 0
// `const T& r = TempProducingExpr();` extends the temp's lifetime
// through r's enclosing scope (N4659 §15.2/6 [class.temporary]).
// Sea-front's mini-block hoist used to scope the temp tighter
// than that — the temp's dtor fired at end of the assignment's
// full-expression, leaving r bound to dead storage for the rest
// of its scope.
//
// Fix: for ref-init binding to a class temp, skip the mini-block
// and push the temp into the OUTER block's cleanup chain so the
// dtor fires at end of r's scope.
//
// Reduced from g++.dg/init/ref19.C.

extern "C" void abort(void);

static int g_dtor_count = 0;

struct A {
    int i;
    ~A() { ++g_dtor_count; }
};

int main() {
    {
        const int &r = A().i;
        /* The temp A's dtor has NOT yet run — lifetime extended
         * through r's scope. Read of `r` (i.e. A.i = 0) is well-
         * defined. */
        if (g_dtor_count != 0) abort();
        (void)r;
    }
    /* Exiting r's scope fires A's dtor exactly once. */
    if (g_dtor_count != 1) abort();
    return 0;
}
