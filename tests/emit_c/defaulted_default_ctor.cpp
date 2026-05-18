// EXPECT: 42
// 'A() = default;' explicitly requests synthesis of the default
// constructor (N4659 §10.1.6.4 [dcl.fct.def.default]). Combined with
// value-init 'A a{}' (§11.6.1/8 [dcl.init]), this must zero-initialize
// the object and then call the (trivial) default constructor.
//
// Sea-front emits a body for the defaulted ctor — under '__SF_INLINE'
// to match the forward-decl linkage. The empty-brace 'A a{}' lowers
// to 'struct sf__A a = {0}; A_ctor(&a);' which gives the C++17
// value-initialization semantics.
//
// Test pattern modelled on g++.dg/cpp0x/initlist-value2.

struct A {
    int i;
    A() = default;
    A(int);   // declared, so 'A() = default;' isn't implicit
};

int main() {
    A a{};
    return (a.i == 0) ? 42 : 1;
}
