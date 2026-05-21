// EXPECT: 0
// Declaration in the condition slot of if / while / for / switch —
// N4659 §9.4.1, §9.4.2, §9.5.1, §9.5.3. Each form creates the named
// variable, tests it (via operator bool / cast-to-int for switch),
// and destroys it at scope exit.
//
// Combined regression for:
//   - ea281e0 (parser: while/for/switch accept the decl form)
//   - 3776f08 (codegen: `T v = T()` rewrites to ctor-call so the
//     ctor actually runs — without it, br is bitwise-zeroed and
//     operator bool reads garbage)
//   - 7015e4b (codegen: synth default-ctor fall-back covers classes
//     where the rewrite hits but no user ctor exists)
//
// scope1 g++.dg covers the full integration; this isolates the
// mechanism so a regression in any single piece is loud.

int ctor_count = 0;
int dtor_count = 0;

struct C {
    int sentinel;
    C() : sentinel(0xC0DE) { ++ctor_count; }
    ~C() {
        // Catch double-dtor / dtor-on-uninitialized via a sentinel.
        if (sentinel != 0xC0DE) return;
        sentinel = 0;
        ++dtor_count;
    }
    operator bool() const { return false; }
};

void use_if()     { if (C br = C())     { return; } }
void use_while()  { while (C br = C())  { return; } }
void use_for()    { for (; C br = C(); ) { return; } }
void use_switch() { switch (C br = C()) { default: return; } }

int main() {
    use_if();      // 1 ctor + 1 dtor
    use_while();   // 1 ctor + 1 dtor
    use_for();     // 1 ctor + 1 dtor
    use_switch();  // 1 ctor + 1 dtor

    if (ctor_count != 4) return 1;
    if (dtor_count != 4) return 2;
    return 0;
}
