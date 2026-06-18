// EXPECT: 0
// N4659 §15.2/3 [class.dtor]: if a destructor invoked during stack
// unwinding exits with an exception, std::terminate is called.
//
// Reproducer pattern: g++.dg/eh/filter2.C.
//   - ex_test() throws e1 (outer unwind begins)
//   - aa's destructor ~a runs as part of cleanup
//   - ~a body throws a NEW e1, caught by `catch (e2&)` — no match
//   - the new e1 escapes ~a → §15.2/3 violation → terminate
//
// Two-part sea-front mechanism:
//
// 1. **Body isolation.** Without it, the body's __SF_CHAIN_THROW
//    macros fire on the inherited THROW state at function entry
//    and skip the body entirely (dtors invoked during unwind would
//    never run their own statements — wrong, dtors EXIST to run
//    during unwind). The dtor wrapper now saves the outer state,
//    clears it to NONE, and restores it at the epilogue.
//
// 2. **Throw-sequence detection.** A monotonic `seq` counter on
//    __sf_exc_state increments in every __SF_THROW_PRIM/CLASS.
//    The dtor wrapper snapshots seq at entry; at exit, if seq
//    advanced AND we were entered during unwind, the body issued
//    a fresh throw that escaped — terminate. Pointer-comparing
//    exc_obj would be unsafe for primitive throws (same literal
//    → same packed pointer); the monotonic counter is the only
//    safe distinguisher.
//
// Gated on `g_cf.func_has_cleanups` — dtors with neither body
// throws nor try/catch don't need the isolation (the outer state
// would just pass through anyway).

extern "C" void abort();
namespace std {
    typedef void (*terminate_handler)();
    terminate_handler set_terminate(terminate_handler);
    void exit(int);
}

struct e1 {};
struct e2 {};

struct a {
    a() {}
    ~a() {
        try { throw e1(); }
        catch (e2 &) {}     // mismatch — e1 escapes ~a
    }
};

void ex_test() {
    a aa;
    try { throw e1(); }
    catch (e2 &) {}         // mismatch — e1 escapes ex_test
}

void my_terminate() { std::exit(0); }

int main() {
    std::set_terminate(my_terminate);
    try { ex_test(); }
    catch (...) {}          // never reached
    abort();                // never reached
}
