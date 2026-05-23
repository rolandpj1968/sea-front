// EXPECT: 0
// __sf_terminate dispatches to a weak fallback definition of
// _ZSt9terminatev (std::terminate). When libstdc++ is linked, its
// strong definition wins per ELF strong-beats-weak rules. When the
// binary doesn't link libstdc++ — as this standalone test does —
// the weak fallback runs and abort()s.
//
// The cproc back-end rejects __attribute__((weak)) on extern
// function *declarations*, only accepting it on definitions. Sea-
// front emits the latter (weak fallback definition + always-call,
// no null check) so the same emitted C compiles on both gcc and
// cproc. Regression for that emit-shape choice.
//
// This test only verifies the happy path: a function with a
// nothrow spec returns normally → __sf_terminate is never called.

void quiet() throw() { return; }

int main() {
    quiet();
    return 0;
}
