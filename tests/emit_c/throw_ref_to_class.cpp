// EXPECT: 0
// `throw refvar` where refvar is `T&` is throwing a T per
// N4659 §8.17/3 [expr.throw] — the operand's reference layer
// is stripped, leaving a class-type throw. Sea-front lowered
// refs to pointers and classified the throw by the LOWERED type,
// so __SF_THROW_PRIM fired with typeinfo_int instead of
// __SF_THROW_CLASS with the class's typeinfo. The handler never
// matched and the cc-side was wrong too — uintptr_t cast of a
// struct value is invalid C.
//
// Both the statement-level throw emit and the expression-level
// throw emit (ternary arms etc.) peel TY_REF / TY_RVALREF before
// the class-vs-prim split. The typeinfo collect walk does the
// same peel so the per-class typeinfo gets emitted.
//
// Reduced from g++.dg/eh/delayslot1.C `throw debug;`.

extern "C" void abort();

struct S {
    int n;
    S(int x) : n(x) {}
    S(const S &o) : n(o.n) {}
};

bool caught_s = false;

void rethrower(S &debug) {
    try {
        throw 1;
    } catch (int) {
        throw debug;     /* class throw via ref operand */
    }
}

int main() {
    S s(42);
    try {
        rethrower(s);
    } catch (S &) {
        /* The thrown S's storage is currently a stack-local of
         * rethrower's frame and is dangling after unwind — reading
         * fields here would surface that separate EH-lifetime gap.
         * For this test, the type match alone is sufficient
         * evidence the ref-peel + class-throw dispatch worked. */
        caught_s = true;
    }
    if (!caught_s) abort();
    return 0;
}
