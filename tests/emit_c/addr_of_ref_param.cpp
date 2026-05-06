// EXPECT: 42
// Taking '&' of a C++ reference parameter MUST lower to the bare
// pointer, not '&(lowered_ptr)'. N4659 §8.3.2 [dcl.ref] — a reference
// is the referent at the language level, so &ref is the address of
// the referent (== the underlying pointer in our lowering).
//
// Sea-front lowers 'Item& r' as 'Item* r'. C++ '&r' must therefore
// emit just 'r', not '&r' (which would yield Item**).
//
// Pattern that surfaced this in production: gengtype-generated
//   void gt_ggc_mx (struct foo_s& x_r) {
//       struct foo_s *x = &x_r;
//       gt_ggc_m_9tree_node ((*x).field);
//   }
// Old emit produced 'struct foo_s* x = (&x_r);' (Foo**), then
// '(*x).field' read the parameter slot as Foo and passed garbage to
// the GC mark walker. cc1plus segfaulted in ggc_collect on bad ptrs.

struct Item { int v; };

static int peek(Item& r) {
    Item *p = &r;          // C++: address of referent
    return p->v;
}

int main() {
    Item it; it.v = 42;
    return peek(it);
}
