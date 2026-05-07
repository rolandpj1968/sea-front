// Parser test for try / catch / throw — N4659 §18 [except] +
// §8.17 [expr.throw]. AST-shape only. Lowering ships in later
// EH slices (see docs/exceptions.md).

int main() {
    try {
        throw 42;
    } catch (int x) {
        return x;
    } catch (...) {
        return -1;
    }

    // Re-throw inside a catch body.
    try {
        throw 7;
    } catch (int) {
        throw;
    }

    return 0;
}
