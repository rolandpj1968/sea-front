// EXPECT: 0
// C++11 attribute-specifier-seq [[noreturn]] permitted in three slots
// of a noptr-declarator (N4659 §11.3/1 and §11.3.5/3):
//   1. leading      : [[noreturn]] void f();
//   2. declarator-id: void f [[noreturn]] ();      <-- libcody internal.hh HCF
//   3. trailing q's : void f() noexcept [[noreturn]];
//
// Sea-front previously only accepted slot 1. Slot 2 broke libcody's
// HCF declaration; slot 3 broke `void g() [[noreturn]];` style.
// Attributes are dropped (no codegen effect) — this test only
// verifies the parser accepts all three forms.

[[noreturn]] void die_a();
void die_b [[noreturn]] ();
void die_c() [[noreturn]];

extern "C" void exit(int);
void die_a() { exit(0); }
void die_b() { exit(0); }
void die_c() { exit(0); }

int main() {
    // Only call die_a so we observe the exit-0 path; the other two
    // exist purely to exercise the parser.
    (void)&die_b; (void)&die_c;
    die_a();
    return 99;  // unreachable
}
