// EXPECT: 42
// C++11 captureless lambda with explicit trailing return type —
// N4659 §8.1.5 [expr.prim.lambda]. Sea-front lowers the lambda body
// to a synthesized static-inline function '__sf_lambda_<N>' at TU
// scope; the lambda expression itself is the function name (decays
// to a fn pointer per §8.1.5/2).
//
// auto deduction then deduces 'int (*)(int)' for the variable, and
// calling it via 'add1(...)' is a regular function-pointer call.
//
// Limitation (deferred): captures and auto-deduced return type fall
// back to the legacy parse-and-discard path that produces ND_NULLPTR.

int main() {
    auto add1 = [](int a) -> int { return a + 1; };
    return add1(41);
}
