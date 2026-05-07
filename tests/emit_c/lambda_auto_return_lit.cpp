// EXPECT: 42
// Lambda with no '->' return type — auto-return deduction
// (N4659 §8.1.5.4/4 + §10.1.7.4 [dcl.spec.auto]). Sea-front does
// parser-side deduction for cheap return-expr shapes (literal,
// scope-resolved ident); complex cases fall back to skip+discard.

int main() {
    auto g = []() { return 42; };  // deduce → int from ND_NUM literal
    return g();
}
