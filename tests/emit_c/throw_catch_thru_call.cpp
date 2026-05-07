// EXPECT: 42
// EH slice 5: cross-function throw propagation. The throw lives in
// a callee (`thrower`), the catch in the caller (`main`).
//
// Per docs/exceptions.md, after every potentially-throwing
// statement the caller must check __sf_exc_state and chain to the
// innermost handler / cleanup label. Slice 5 emits this
// __SF_CHAIN_THROW conservatively after every expression-
// statement in a function that has the unwind machinery; phase 3+
// noexcept inference will narrow it back to provably-throwing
// callees.

void thrower() {
    throw 42;
}

int main() {
    try {
        thrower();
    } catch (int x) {
        return x;
    }
    return -1;
}
