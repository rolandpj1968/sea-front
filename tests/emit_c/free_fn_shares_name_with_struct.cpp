// EXPECT: 42
// A free function whose name also names a struct type must be emitted
// as a free function — not as a class method of the struct. The parser's
// declarator-id name lookup finds the struct tag by name and resolves
// its class_region; that result must only apply when the declarator-id
// is qualified ('Foo::bar'). For bare 'inline_edge_summary(...)' the
// stashed class scope would otherwise propagate into the function-def
// branch and mis-tag the function as an OOL method, mangling it as
// 'sf__inline_edge_summary__inline_edge_summary_p_..._pe_' while the
// call site emits the name unmangled.
//
// Pattern from gcc 4.8 ipa-inline.h:
//   struct inline_edge_summary { int call_stmt_size; ... };
//   inline_summary_t *inline_edge_summary_vec;
//   static inline struct inline_edge_summary *
//   inline_edge_summary (struct cgraph_edge *edge) { ... }
// N4659 §6.4.3 [basic.lookup.qual]: only qualified-ids name out-of-class
// members.

struct inline_edge_summary {
    int call_stmt_size;
};

struct cgraph_edge {
    struct inline_edge_summary summary;
};

static inline struct inline_edge_summary *
inline_edge_summary(struct cgraph_edge *edge) {
    return &edge->summary;
}

int main() {
    struct cgraph_edge e;
    e.summary.call_stmt_size = 42;
    return inline_edge_summary(&e)->call_stmt_size;
}
