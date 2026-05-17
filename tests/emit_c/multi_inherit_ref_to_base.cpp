// EXPECT: 7
// Multi-inheritance: passing a derived object where a base reference
// is expected must bind to the correct sub-object — N4659 §11.6.3
// [dcl.init.ref]/4.1 + §7.11 [conv.ptr]. In sea-front's layout the
// Nth base lives at __sf_base[N], so 'D d; takes_B(d)' must emit
// '&(d).__sf_base1' not '&(d)' — otherwise the callee reads past
// the wrong sub-object.

struct A { int va; };
struct B { int vb; };

struct D : A, B { };

int takes_B(B &b) { return b.vb; }

int main() {
    D d = { {3}, {7} };
    return takes_B(d);     // binds B& to d's B subobject — vb == 7
}
