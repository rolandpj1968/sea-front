// Demo: exceptions via TLS polling (see docs/exceptions.md).
//
// Sea-front does NOT use Itanium _Unwind_RaiseException. Instead, a
// thread-local __sf_exc_state holds the in-flight exception, and the
// existing __SF_unwind / goto-chain machinery is reused: throw sets the
// state and chains to the innermost handler/cleanup label; after every
// potentially-throwing statement, the caller polls __sf_exc_state and
// chains again if it's still set.
//
// Benefits: no libunwind dependency, no DWARF tables, runs anywhere a
// freestanding C compiler can target. Cost: explicit polling at call
// sites (which a noexcept-inference pass can later prune).

int thrower(int v) {
    if (v < 0) throw 42;
    return v;
}

int main() {
    int total = 0;
    try {
        total = total + thrower(10);     // 10
        total = total + thrower(-1);     // throws 42; total stays 10
        total = total + thrower(99);     // not reached
    } catch (int x) {
        return total + x;                // 10 + 42 = 52
    }
    return -1;
}
