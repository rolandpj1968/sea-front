// EXPECT: 42
// N4659 §8.5.4/3 [dcl.init.list]: a brace-init-list with a single
// element implicitly converts to a scalar target. `f({42})` calls
// f(int) with 42; `return {7};` returns 7. C has no `{...}`
// expression form, so emit must unwrap the braces at call args
// and return statements.
//
// Reduced from g++.dg/cpp0x/initlist16.C.

extern "C" void abort(void);

int g_arg;
void f(int i) { g_arg = i; }
int  h(int)   { return {7}; }   // scalar return from brace-list
int  z()      { return {}; }    // empty brace-list — value-init 0

int main() {
    f({42});                    // unwrap to f(42)
    if (g_arg != 42) abort();
    if (h(0) != 7) abort();
    if (z() != 0) abort();
    return {42};                // unwrap to return 42
}
