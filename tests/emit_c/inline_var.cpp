// EXPECT: 42
// C++17 inline variables — N4659 §10.1.6/7 [dcl.inline]: same
// multi-TU dedup guarantees as inline functions. Sea-front lowers
// 'inline T x = init;' at namespace scope to 'static T x = init;'
// so a header that defines such a variable can be included by
// multiple TUs without multi-definition link errors.
//
// Single-TU run is the baseline check here; the multi-TU test
// suite covers cross-TU dedup separately.

inline int magic = 42;
int main() { return magic; }
