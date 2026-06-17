// EXPECT: 42
// Captureless lambda synthesised inside a template body — N4659
// §8.1.5 [expr.prim.lambda] + §17.6.4 [temp.point]. The lambda fn
// must be cloned per instantiation (the body may contain
// substituted types from the template), and its emitted symbol
// must match what the call site references.
//
// Pre-fix: the parser deferred TU-top hoist for templates, but the
// captureless return path returned a bare ND_IDENT that didn't
// carry the func_def — so the instantiation walker had nothing to
// clone. The call site then referenced an undefined symbol.

template<int N>
int func() {
    return [] { return 42; }();
}

int main() {
    return func<1>();
}
