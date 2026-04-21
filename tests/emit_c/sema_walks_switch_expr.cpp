// EXPECT: 3
// Sema must walk into switch/case/default subexpressions. Previously
// ND_SWITCH (and ND_CASE, ND_DEFAULT) were hit by the 'default:
// return;' in the visit dispatch, so identifiers and method calls
// inside 'switch(expr)' never got resolved_type / resolved_decl set.
// Method dispatch at codegen then fell through to field-access
// ('.method()' as a struct field in C), tripping 'struct has no
// member' errors. Pattern from gcc 4.8 dwarf2cfi.c create_cie_data:
//   switch (cie_trace.regs_saved_in_regs.length())
struct Counter {
    int n;
    Counter() : n(0) {}
    int value() const { return n; }
};

int main() {
    Counter c;
    c.n = 3;
    switch (c.value()) {
    case 0: return 10;
    case 3: return c.value();
    default: return 99;
    }
}
