// EXPECT: 42
// for-init declaration of a pointer variable whose name shadows the
// pointed-to struct's tag. The cond expression 'loop != X' must be
// recognised as plain pointer comparison, NOT dispatched as the struct
// type's overloaded operator!= — the variable 'loop' (struct loop *)
// is in scope inside the cond, not the type 'struct loop'.
//
// The bug this guards against: parse_for_stmt pushed a REGION_BLOCK
// for the for-init scope, registered the variable in it, then popped
// the region without storing it on the ND_FOR node. Sema's visit_for
// didn't re-enter the scope, so 'loop' looked up only the outer
// scope's type tag — codegen mis-classified 'loop != X' as a class
// operator overload and emitted 'sf__loop__ne(&loop, X)'.
//
// Pattern from gcc 4.8 tree-ssa-loop-manip.c:
//   for (struct loop *loop = def_loop; loop != current_loops->tree_root; ...)
//
// Standard: N4659 §6.3.3/4 [basic.scope.block] — "Names declared in
// the init-statement ... are local to the for statement."

struct loop { int n; };

int main() {
    struct loop sentinel;
    sentinel.n = -1;
    struct loop x;
    x.n = 42;
    struct loop *root = &x;
    struct loop *end = &sentinel;
    int total = 0;
    for (struct loop *loop = root; loop != end; loop = end) {
        total = loop->n;
    }
    return total;
}
