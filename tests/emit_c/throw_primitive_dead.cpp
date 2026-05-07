// EXPECT: 0
// EH slice 3: throw lowering for primitive types — N4659 §8.17
// [expr.throw] + §18 [except], lowering per docs/exceptions.md.
//
// The throw path here is dead (the if-condition is false), so the
// runtime never actually triggers a throw. The test verifies that
// (a) the throw-expression lowers to valid portable C — the
// __SF_THROW_PRIM macro invocation compiles and links — and (b)
// the function still has a normal prologue/epilogue so the
// non-throwing path returns 0 cleanly.
//
// Catch-side is in slice 4; cross-function propagation in slice 5.
// Until then a real throw would set __sf_exc_state.exc_type and
// goto __SF_epilogue, returning whatever __SF_retval happens to
// hold — silent miscompile rather than std::terminate. That's
// acceptable here because the path is unreachable.

int main() {
    int i = 0;
    if (i != 0) throw 42;       // dead branch
    return i;
}
