// EXPECT: 7
// Anonymous union at function scope — 'static union { int i; };'.
// C11 6.7.2.1/13 covers anonymous struct/union *members* of a
// struct/union; gcc admits the same syntax at function/file scope as
// an extension, but gcc-as-C still requires that the anonymous union
// have *some* instance — it doesn't accept a bare 'union { int i; };'.
//
// Sea-front lowers function-scope anon unions to a synthesised-
// instance form '__sf_anon_inst_<id>' and rewrites bare references
// to anon-union members to instance-qualified access. Storage class
// from the decl-specifier seq (here 'static') is preserved so the
// union has the right linkage and lifetime.
//
// Test pattern modelled on g++.dg/abi/pr39188-3 (the static-int
// variant — the inline variants depend on cross-TU inline ODR which
// is a separate concern).

static int f(int x) {
    static union {
        int i;
    };
    int j = i;
    i = x;
    return j;
}

int main() {
    // f(7): j = 0 (initial), i = 7, return 0
    // f(0): j = 7 (preserved), i = 0, return 7
    return f(7) + f(0);
}
