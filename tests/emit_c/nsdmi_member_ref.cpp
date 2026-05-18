// EXPECT: 42
// NSDMI (non-static data member initializer) referencing another
// member of the same class — N4659 §12.6.2/9 [class.base.init].
// The init expression is evaluated in the ctor's body, so member
// references must resolve as 'this->member'. Sea-front's sema was
// not pushing the class region as cur_scope while visiting member
// initializers, so the lookup of 'x' in 'int j = x' fell through to
// the file scope and the bare ident was emitted unrewritten —
// 'this->j = x;' fails to compile against the C struct.
//
// Test pattern modelled on the same-class fragment of the nsdmi
// tests in g++.dg/cpp0x.

struct B {
    int x;
    int j = x + 1;
    B(int v) : x(v) {}
};

struct D : B {
    int k = x;            // inherited member, non-virtual base
    D(int v) : B(v) {}
};

int main() {
    D d(41);
    return (d.j == 42 && d.k == 41) ? 42 : 1;
}
