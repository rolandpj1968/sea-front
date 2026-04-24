// EXPECT: 42
// Non-lvalue (call result) used in an address-taking position must be
// materialized to a named temporary before the '&' emit. Three shapes
// in gcc 4.8 that hit this:
//
//   1. struct-returning call as binary-op lhs:
//        make_val() == other  →  sf__T__eq(&make_val(), other)   (BAD)
//      Fix: hoist make_val() to __SF_temp_N first.
//
//   2. single-stmt if body with a method call taking a ref arg:
//        if (cond) v.push(call_returning_ptr())
//      The ref-param lowering emits '&(call_returning_ptr())'.
//      The if-body is not a block, so the hoist had no valid scope.
//      Fix: wrap single-stmt bodies in '{ }' for hoist scope.
//
// Pattern: gcc 4.8 cgraph.c cgraph_add_thunk and cgraphunit.c
// assemble_thunk. N4659 §11.3.2 [dcl.ref].

struct Val {
    int v;
    bool operator==(const Val &rhs) const { return v == rhs.v; }
};

static Val make_val(int x) { Val r; r.v = x; return r; }

struct Box {
    Val v;
    void set(Val &r) { v = r; }
};

int main() {
    Val a; a.v = 42;
    Box b;
    bool eq = make_val(42) == a;        // hoist lhs call for operator==
    if (eq) {
        b.set(make_val(a.v));           // hoist call arg for ref param
        return b.v.v;
    }
    return 0;
}
