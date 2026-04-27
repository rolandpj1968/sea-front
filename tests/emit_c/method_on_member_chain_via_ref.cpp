// EXPECT: 0
// N4659 §6.4.5 [class.qual] — method dispatch through a chained
// member access where the leading object is a ref-to-pointer
// parameter: 'v->member.method()' where v is T*&.
//
// Earlier #134 fix handled the simple form (sema-set resolved_type
// missing on the inner ND_MEMBER). This test covers the deeper
// case where the leading ident is a ref-param to a TEMPLATE-
// INSTANTIATED class, whose Type copy passing through subst_type
// has class_region NULL — the codegen fallback now walks the TU
// by (tag, template_args) to recover the canonical class_def
// whose Type has class_region set.
//
// Surfaced by gcc 4.8 vec.h va_heap::release/reserve which call
// '(*v)->vecpfx_.release_overhead()' inside cloned member-template
// bodies. Without the fix, .release_overhead() got emitted as a
// struct field access ('vecpfx_.release_overhead()') and the C
// compile errored on "no member named release_overhead".
struct VecPrefix {
    int alloc;
    void release_overhead() { alloc = 0; }
};

template<typename T>
struct Vec {
    VecPrefix vecpfx_;
    T data;
};

template<typename T>
void release(Vec<T>*& v) {
    if (v) v->vecpfx_.release_overhead();
}

int main() {
    Vec<int> v;
    v.vecpfx_.alloc = 42;
    v.data = 7;
    Vec<int> *p = &v;
    release<int>(p);
    return v.vecpfx_.alloc == 0 ? 0 : 1;
}
