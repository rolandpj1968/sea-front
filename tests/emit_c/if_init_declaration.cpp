// EXPECT: 42
// C++ if-with-init-declaration:
//   if (T *v = expr) { use(v); }
// N4659 §9.4.1 [stmt.select]/2. Parser stores an ND_VAR_DECL as the
// if_.cond. Codegen previously emitted it via emit_expr which fell
// through to the '/* expr */' default → C syntax error ('expected
// expression before )').
//
// Fix: lift the declaration into an enclosing block and test the
// variable's value:
//   { T *v = expr; if (v) { ... } }
// The block keeps the scope right (v visible in then/else only).
//
// Pattern from gcc 4.8 cgraph.h's varpool_first_variable:
//   if (varpool_node *vnode = dyn_cast<varpool_node>(node)) return vnode;
struct Thing {
    int v;
    int get() const { return v; }
};

Thing* maybe(int v) {
    static Thing t;
    t.v = v;
    return v > 0 ? &t : (Thing*)0;
}

int main() {
    if (Thing *p = maybe(42))
        return p->get();
    return 0;
}
