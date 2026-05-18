// EXPECT: 42
// Virtual call through a reference parameter — 'x.m()' where 'x'
// is 'const B &'. Sea-front lowers the parameter to 'const B *'
// in C; the virtual call has to be 'x->__sf_vptr->m(x)', not
// '(*x)->__sf_vptr->m(x)'. emit_ident's default behavior auto-derefs
// ref-params, so the virtual-dispatch emit path must suppress that
// deref for the receiver so '->' lands on a pointer rather than a
// struct value.
//
// Test pattern modelled on the 'x.m()' calls in g++.dg/torture/pr48661.C.

struct B {
    virtual int m() { return 42; }
};

int call(const B &x) { return x.m(); }

int main() {
    B b;
    return call(b);
}
