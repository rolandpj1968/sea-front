// EXPECT: 42
// EH slice 4: try/catch landing pad for primitive types — N4659
// §18 [except] + §8.17 [expr.throw], lowering per docs/exceptions.md.
//
// Canonical end-to-end test: 'throw 42' inside a try-block lowered
// to __SF_THROW_PRIM, dispatched by __SF_try_<N>_handler against
// __sf_typeinfo_int, bound to the catch-parameter, returned as the
// function's value. Validates the full runtime correctness loop:
// state set → goto handler label → type-id pointer compare →
// payload cast back from uintptr_t → bind to named local → handler
// body executes → state cleared on catch → fall through cleanly.

int main() {
    try {
        throw 42;
    } catch (int x) {
        return x;
    }
    return -1;
}
