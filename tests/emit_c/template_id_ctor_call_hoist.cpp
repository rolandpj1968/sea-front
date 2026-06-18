// EXPECT: 0
// Class-template functional-cast `A<T>(args)` as a call argument
// passed to a const-ref param. The argument is a class-typed
// rvalue from a ctor call — sea-front must hoist into a named
// temp so the caller's `&(arg)` lowering for ref params has an
// addressable lvalue.
//
// is_class_temp_call recognises ND_TEMPLATE_ID callees (not just
// ND_IDENT) and hoist_emit_decl's is_ctor_call detection
// synthesises ctor_class_type from the template-id via
// find_class_def_by_tag_args when sema didn't stamp
// resolved_type on the call.
//
// Reduced from g++.dg/other/friend2.C `foo(A<unsigned>(0))`.

extern "C" void abort();

template<typename T>
struct A {
    T x;
    A(T t) : x(t) {}
};

int sum(const A<unsigned> &a, const A<int> &b) {
    return (int)a.x + b.x;
}

int main() {
    /* Both args are class-template functional-cast rvalues; each
     * needs hoisting separately. */
    if (sum(A<unsigned>(40), A<int>(2)) != 42) abort();
    return 0;
}
