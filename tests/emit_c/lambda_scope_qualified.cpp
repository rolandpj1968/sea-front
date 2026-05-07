// EXPECT: 24
// Scope-qualified lambda naming — symbols are
// '__sf_lambda_<enclosing>__<N>' (and matching closure tags)
// with N reset per enclosing function. Two consequences this
// test verifies:
//  - Two functions can each have a lambda counted as 0 — no
//    cross-function collision; the enclosing-fn name disambiguates.
//  - Multiple lambdas in main get N=0, N=1 — stable across edits
//    that add lambdas to OTHER functions.

int helper(int x) {
    auto h = [x]() -> int { return x * 2; };  // __sf_lambda_helper__0
    return h();
}
int main() {
    auto f = []() -> int { return 4; };        // __sf_lambda_main__0
    auto g = [&]() -> int { return helper(10); };  // __sf_lambda_main__1
    return f() + g();   // 4 + helper(10) = 4 + 20 = 24
}
