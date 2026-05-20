// EXPECT: 0
// GNU function attribute pass-through: `__attribute__((constructor))`
// makes the function run before main() (via .init_array). Mirrors
// destructor as the after-main hook. Not in N4659 — gcc extension —
// but pervasive in real code (gcc tree-pass init, glibc ctors, etc.).
// Pattern: g++.dg/init/attrib1.C.
//
// The parser captures the bare-identifier attribute name during
// consume_trailing_qualifiers (after the function-shape declarator)
// and stamps a flag on the var_decl. Codegen emits
// `__attribute__((constructor))` ahead of the C prototype; the cc
// backend wires the function into .init_array.

int g_ran_ctor = 0;
int g_ran_dtor = 0;

void on_load() __attribute__((constructor));
void on_load() { g_ran_ctor = 1; }

void on_unload() __attribute__((destructor));
void on_unload() { g_ran_dtor = 1; }

int main() {
    // g_ran_ctor must already be 1 here (set by .init_array hook).
    return g_ran_ctor == 1 ? 0 : 1;
}
