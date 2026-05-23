// EXPECT: 0
// `__attribute__((weak))` on a free-function PROTOTYPE must pass
// through to the emitted C declaration. Pre-fix only ND_FUNC_DEF
// emitted the attribute; for declarations (`extern void f()
// __attribute__((weak));`) the attribute was dropped and the
// linker errored on the unsatisfied symbol.
//
// Pattern: g++.dg/warn/weak1.C.

extern void undefined_weak_fn() __attribute__((weak));

int main() {
    // If &undefined_weak_fn is null (no def at link), don't call.
    // If the proto's weak attr was dropped, the linker would
    // error out at link time instead of leaving the symbol null
    // at run time.
    if (&undefined_weak_fn) undefined_weak_fn();
    return 0;
}
