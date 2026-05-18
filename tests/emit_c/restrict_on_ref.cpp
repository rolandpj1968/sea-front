// EXPECT: 0
// __restrict on a reference parameter — N4659 §11.3.2/1 [dcl.ref]
// disallows cv-qualified references, but gcc accepts __restrict on
// them as a hint. libstdc++ 13's cxxabi.h uses it on the upcast/
// find_public_src helpers, blocking every dg test that touches RTTI
// through abi/* until the parser tolerates it.
//
// __restrict tokenises to TK_KW_VOLATILE (see lex/tokenize.c). The
// declarator parser discards trailing const/volatile after '&' / '&&'
// to match the gcc-accepted shape.

struct R { int v; };

void f(R& __restrict r) { r.v = 42; }

int main() {
    R r = { 0 };
    f(r);
    return r.v == 42 ? 0 : 1;
}
