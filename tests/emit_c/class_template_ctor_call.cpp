// EXPECT: 0
// Class template with functional-cast / value-init syntax used both as
// a constructor call AND as a type. gcc 4.8 ipa-cp.c pattern:
//   vec<T,A,L> make() { return vec<T,A,L>(); }  // ctor-call form
//   vec<T,A,L> x;                                // type-position use
//
// Two issues collided:
// 1. The instantiation collector created a request with usage_type=NULL
//    for the ctor-call form. The dedup_add was nested inside the
//    usage_type != NULL guard, so this request didn't register. The
//    subsequent type-position request found no existing entry and
//    instantiated the class a SECOND time — producing duplicate
//    ND_CLASS_DEF nodes whose methods defined the same symbol twice.
// 2. 'vec<T,A,L>()' callee is ND_TEMPLATE_ID, not ND_IDENT. emit_expr
//    had no branch for it — emitted as '/* expr */()' stub.
//
// Fix 1: hoist dedup_add out of the usage_type guard.
// Fix 2: handle ND_TEMPLATE_ID-callee 0-arg calls as compound-literal
// '(struct sf__T...){0}' zero-init.

template<typename T, typename A = int>
struct Box {
    T val;
    A meta;
};

Box<int, char> make_empty() {
    return Box<int, char>();   // value-init via functional cast
}

Box<int, char> stored;         // same instantiation as type

int main() {
    stored = make_empty();
    return stored.val + stored.meta;
}
