// EXPECT: 7
// Sibling member-template call with EXPLICIT template args, made
// from inside another member-template's body. Pattern from gcc 4.8
// hash-table.h: hash_table::traverse calls
// `traverse_noresize<Argument, Callback>(argument)` unqualified —
// where both `Argument` and `Callback` are template-parameters of
// the calling traverse template.
//
// Sea-front's collection path for unqualified sibling-member-template
// calls only matched ND_IDENT callees — explicit `<args>` made the
// callee ND_TEMPLATE_ID and missed collection. The instantiation pass
// produced no body and codegen fell through to the default
// ND_TEMPLATE_ID emit, which prints just the bare name. Link broke.
//
// Fix: extend the sibling path to ND_TEMPLATE_ID callees too. After
// instantiation, reduce the call-site callee to ND_IDENT with
// implicit_this set so codegen mangles via the standard class-method
// dispatch. N4659 §6.4.1 [basic.lookup.unqual] / §17.5.2 [temp.mem].

template <typename T>
struct holder {
    T x_;

    template <typename U>
    U inner(U u) { return u + x_; }

    template <typename U>
    U outer(U u) { return inner<U>(u); }   // sibling member-template, explicit args
};

int main() {
    holder<int> h;
    h.x_ = 5;
    return h.outer<int>(2);  // 5 + 2 = 7
}
