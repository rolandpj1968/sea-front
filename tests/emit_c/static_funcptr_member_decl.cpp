// EXPECT: 0
// A static class member of function-pointer type — `static void
// (*handler)();` — must emit the C declaration with the name
// INSIDE the grouped declarator: `void (*sf__T__handler)(void);`,
// not the invalid `void (*)(void) sf__T__handler;` (which cc
// rejects with "expected identifier or '(' before ')' token").
// Both the cross-TU weak in-class shadow and the OOL definition
// take this path. N4659 §11.3.5 [dcl.fct] + §11.4.9.2
// [class.static.data].
//
// Reduced from g++.dg/init/static3.C — the OOL definition with
// an in-init reference and the `T::handler()` indirect-call shape
// are separate slices; this test only covers the declaration shape.

extern "C" void abort();

bool ran = false;

void my_handler() { ran = true; }

struct T {
    static void (*handler)();
};

void (*T::handler)();

int main() {
    T::handler = my_handler;
    (*T::handler)();
    if (!ran) abort();
    return 0;
}
