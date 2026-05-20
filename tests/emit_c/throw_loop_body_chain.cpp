// EXPECT: 0
// EH: a throwing call inside a single-statement loop body needs the
// __SF_CHAIN_THROW emitted INSIDE the loop body, not at the
// enclosing scope. Without braces around the body, the chain-throw
// landed one level out and the throw was lost until after the loop
// completed.
//
// Regression for g++.dg/eh/loop1.C — `for (...) bar(ptr);` where
// bar() throws int and the catch outside expects to see i==0 (no
// loop iterations completed). Fix: for/while/do bodies are
// brace-wrapped when func_has_cleanups and the body is non-block.

static void bar() { throw 1; }

int main() {
    unsigned long i = 1;
    try {
        for (i = 0; i < 10; i++) bar();
    } catch (...) {
        // bar throws on first iteration — i must still be 0
        return i == 0 ? 0 : 1;
    }
    return 2;
}
