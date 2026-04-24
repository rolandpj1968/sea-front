// EXPECT: 42
// A labeled statement (N4659 §9.1 [stmt.label]) was never visited by
// sema's Stmt walker: the switch had ND_SWITCH/ND_CASE/ND_DEFAULT but
// no ND_LABEL case, so identifiers inside the labeled body never got
// resolved_type set. Downstream, method dispatch (obj.method())
// couldn't see obj's class type and emit_expr fell through to literal
// C member access ('obj.method(args)') which is invalid for a method.
// Pattern: gcc 4.8 cfgexpand.c expand_used_vars — 'next: if (...)
// maybe_local_decls.safe_push(var);' where 'next' is a goto target.

struct Box {
    int v;
    void set(int x) { v = x; }
};

int main() {
    Box b;
next:
    if (1) {
        b.set(42);          // method call inside labeled-stmt body
    }
    return b.v;
}
