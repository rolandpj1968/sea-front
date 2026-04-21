// EXPECT: 7
// When a reference parameter is used as the receiver of a method
// call, the generated code must pass the ref (already a pointer in
// our lowering) directly — not take its address. Previously we emitted
// '&(*r)' which happens to work for strict aliasing but produced the
// wrong pointer for inherited methods and was fragile for operator
// overloads. N4659 §11.3.2 [dcl.ref].
struct X {
    int v;
    int get() const { return v; }
};

int call_through_ref(const X& r) { return r.get(); }
int call_through_ptr(const X* p) { return p->get(); }

int main() {
    X x;
    x.v = 7;
    return call_through_ref(x) + call_through_ptr(&x) - x.v;  // 7+7-7=7
}
