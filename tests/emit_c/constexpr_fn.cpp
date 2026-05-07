// EXPECT: 42
// 'constexpr' function — N4659 §10.1.5/1 [dcl.constexpr]: a constexpr
// function is implicitly inline. Sea-front lowers as 'static inline'
// (per-TU body, dead-code-eliminated when unused) so header-defined
// constexpr fns dedupe across TUs without runtime cost when unused.
// We don't enforce constexpr's compile-time-evaluability restrictions
// (§10.1.5/3) — the call here happens at runtime.

constexpr int square(int x) { return x * x; }
int main() { return square(6) + 6; }
