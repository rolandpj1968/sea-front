// EXPECT: 0
// A class template with an empty-bodied virtual destructor must
// still get its dtor / __del_dtor wrappers synthesized — the vtable
// forward-declares them and the linker errors with "undefined
// reference to ...__del_dtor" otherwise.
//
// Sea-front's template-instantiation per-class flag recomputation
// in instantiate.c set has_dtor=false when the user dtor body was
// empty, which skipped dtor synthesis entirely. The corresponding
// non-template path in parse/type.c includes a `|| m->func.is_virtual`
// clause for exactly this case; this fix mirrors it.
//
// Pattern: g++.dg/torture/pr44535.C
// `template<T> class A { virtual ~A() { } virtual void f() = 0; };`

extern "C" void abort();

int dtor_calls = 0;

template<typename T>
struct Base {
    int v;
    Base() : v(0) { }
    virtual ~Base() { }
};

int main() {
    Base<int> *p = new Base<int>;
    p->v = 5;
    if (p->v != 5) abort();
    delete p;       // dispatches through vtable's __del_dtor slot
    return 0;
}
