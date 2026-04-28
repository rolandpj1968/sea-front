// EXPECT: 14
// Regression for d6f895e + 1f5ad77 (c_linkage propagates across
// decls of the same name when ALL visible decls share one
// signature).
//
// Real headers mix extern "C" decls and non-extern-C definitions
// of the same function. libcpp's internal.h declares
// _cpp_lex_token inside extern "C"; lex.c then defines it
// without the wrapper. Per-decl c_linkage made the def emit
// MANGLED while callers (whose view came from the extern "C"
// header decl) emitted the bare name — 70+ unresolved cpp_*
// refs in cc1plus.
//
// 1f5ad77 refined the rule: propagate c_linkage only when ALL
// visible decls share one signature, so genuine C++ overloads
// (e.g. stdlib's extern "C" abs(int) + libstdc++'s abs(long),
// abs(double), …) still mangle by sig.
//
// N4659 §10.5/6: a name with C linkage refers to a single C
// function regardless of how many decls exist.

extern "C" {
    int my_op(int x);    // C-linkage decl
}

// Definition WITHOUT an extern "C" wrapper. Same sig as the
// decl above. Without d6f895e, this def emits mangled while the
// caller below (seeing the extern "C" decl) emits bare → link
// fails inside the same TU.
int my_op(int x) { return x * 2; }

int main() {
    return my_op(7);   // 7 * 2 = 14
}
