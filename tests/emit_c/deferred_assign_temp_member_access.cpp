// EXPECT: 0
// A struct-returning call hoisted inside a short-circuit branch
// gets a DEFERRED assignment: sea-front emits `T __SF_temp_N;`
// without an initializer, then expects the use site to gate the
// call. Pre-fix the deferred-assign mechanism only fired for
// `&obj` (implicit-this binding); plain reads via `.member` left
// the temp uninitialized.
//
// Pattern: g++.dg/opt/expect1.C — `bar().i == 0` inside a
// __builtin_expect call.

extern "C" void abort();

struct Y { int i; };

Y bar() {
    Y y = { 0 };
    return y;
}

bool foo() { return true; }

int main() {
    // bar() is hoisted because it's inside the RHS of && (short-
    // circuit). Pre-fix __SF_temp_0.i read undef memory and the
    // condition was non-deterministic.
    if (!(foo() && bar().i == 0)) abort();
    return 0;
}
