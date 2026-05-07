// EXPECT: 42
// 'constexpr' variable — N4659 §10.1.5/9 [dcl.constexpr]: a constexpr
// variable is implicitly const. Sea-front lowers TU-scope constexpr
// vars as 'static const' so headers can be included by multiple TUs
// without multi-definition link errors (C++ §3.5/3 already gives
// const namespace-scope vars internal linkage by default; we just
// make that explicit in C).

constexpr int magic = 42;
int main() { return magic; }
