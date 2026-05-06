// EXPECT: 42
// C++11 'auto' type deduction — N4659 §10.1.7.4 [dcl.spec.auto].
// Sea-front's parser emits TY_INT with is_auto=true; sema deduces
// from the initializer in visit_var_decl. Three forms:
//   auto  x = e;   T = type-of(e), refs stripped
//   auto& x = e;   T = type-of(e), refs stripped; outer stays ref
//   auto* x = e;   T = pointee-of(e); outer stays ptr
//
// Also exercises sema's address-of: '&ref_param' must yield T* (not
// T&*) — there are no pointers to references in C++ (§11.3.1/1).

struct Box { int v; };

static int reader(const Box& b) {
    auto x = b.v;          // int
    auto& r = b;           // const Box& -- ref-to-ref binding
    const auto& cr = b;    // const Box&
    auto* p = &b;          // const Box*  -- requires &ref strip
    return x + r.v + cr.v + p->v - 126;  // 42*4 - 126 = 42
}

int main() {
    Box b; b.v = 42;
    return reader(b);
}
