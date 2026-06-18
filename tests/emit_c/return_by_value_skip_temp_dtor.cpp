// EXPECT: 0
// Class-type return-by-value: the IN-FUNCTION cleanup must NOT
// destroy the temp being returned — the caller takes ownership of
// the temp's destruction at end of the caller's full-expression.
// Sea-front had NRVO-style move-skip-cleanup logic for NAMED locals
// (`return t;`) but the comparison used `var_decl.name` on the
// live[] entry; for hoisted CALL temps the entry's `var_decl` is
// actually the ND_CALL node and reading its var_decl.name union
// slot was UB. So `return A();` (synthesized __SF_temp_0) skipped
// the move detection, fired ~A() in callee's cleanup, and the
// caller-side dtor on the returned-and-discarded temp fired a
// second time — net `--c` twice for a single logical object.
//
// Fix: detect ND_CALL hoist-temps via `codegen_temp_name` instead
// of `var_decl.name`. Then `__SF_RETURN(temp, ...)` jumps direct
// to __SF_epilogue, skipping the dtor that would have run on the
// return path; throw/fall-through paths still hit the dtor.
//
// Pattern: g++.dg/init/ref16.C `const A& ar = i ? *ap : f();` —
// previously xfailed for this exact reason; now PASSes.

extern "C" void abort();

int c = 0;

struct A {
    int v;
    A()         : v(1) { ++c; }
    A(int x)    : v(x) { ++c; }
    A(const A& o) : v(o.v) { ++c; }    /* shouldn't fire — NRVO move */
    ~A() { --c; }
};

A make_a()      { return A(); }
A make_a(int x) { return A(x); }

int main() {
    /* Single call, value discarded — temp lives through end of
     * full-expression at the caller, then dies. Net c delta = 0. */
    make_a();
    if (c != 0) abort();

    /* Bind to a const ref — temp lives through ar's scope. */
    {
        const A &ar = make_a(42);
        if (ar.v != 42) abort();
        if (c != 1) abort();   /* temp alive */
    }
    if (c != 0) abort();   /* temp destroyed at block exit */

    /* Bind to value — copy ctor runs once at caller, temp dies. */
    {
        A copy = make_a(7);
        if (copy.v != 7) abort();
        if (c != 1) abort();   /* either the returned temp or its copy is alive */
    }
    if (c != 0) abort();

    return 0;
}
