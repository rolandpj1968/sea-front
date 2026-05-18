// EXPECT: 0
// Functional-cast temporary 'T()' for a class with no ctor —
// N4659 §11.6/8 [dcl.init] value-initialization: zero-init the
// object's storage when no ctor matches.
//
// Sea-front hoists 'foo()' in expression position into a synthetic
// local 'struct foo __SF_temp_N;' and then calls the ctor. Before
// the fix, when resolve_overload returned no match (no default ctor
// for the trivial type), the temp stayed uninitialized — pointer
// members held indeterminate values, breaking equality comparisons
// against zero-init'd siblings.
//
// Real-world: g++.dg/init/ptrmem4.C pattern (ptmf member compared
// against foo().mem2).

struct foo {
    int   mem1;
    int (foo::*mem2);  // pointer-to-member; zero is NULL ptmf
};

int main() {
    // Stamp the stack first so 'lucky' indeterminate-zero won't
    // hide a regression.
    int volatile noise[64];
    for (int i = 0; i < 64; i++) noise[i] = (int)0xCDCDCDCDu;
    (void)noise;

    foo x = { 0 };
    if (x.mem2 != foo().mem2) return 1;
    return 0;
}
