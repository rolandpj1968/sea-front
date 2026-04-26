// EXPECT: 0
// N4659 §6.4.5 [class.qual] — looking up a member function through a
// chain like 'v->inner.bump()' where v is T*&. Sema can leave the
// inner ND_MEMBER's resolved_type unset for this shape; codegen's
// method-dispatch fallback now resolves the field type by looking it
// up in the outer's class_region. Surfaced by gcc 4.8 vec.h
// va_heap::reserve emitting '(*v)->vecpfx_.release_overhead()'.
struct Inner {
    int n;
    void bump() { ++n; }
};
struct Outer { Inner inner; };

void run(Outer*& v) { v->inner.bump(); }

int main() {
    Outer o; o.inner.n = 0;
    Outer *p = &o;
    run(p);
    return o.inner.n == 1 ? 0 : 1;
}
