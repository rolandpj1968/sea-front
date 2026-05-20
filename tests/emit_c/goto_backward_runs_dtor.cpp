// EXPECT: 0
// `goto` that crosses out of a class-typed local's scope must fire
// the dtor — N4659 §6.7/2 [stmt.jump]: "On exit from a scope ...
// destructors are called for all constructed objects with automatic
// storage duration ... declared in that scope, in the reverse order
// of their construction." A backward goto past a labelled decl
// re-enters the scope on the way; the var is destroyed first.
//
// Two pieces were broken:
//   1. subtree_has_cleanups didn't descend into ND_LABEL / ND_CASE /
//      ND_DEFAULT / ND_SWITCH, so a labelled class-typed local
//      didn't flag the enclosing function as func_has_cleanups —
//      no cleanup machinery was emitted at all.
//   2. push_user_var_cleanup didn't look through ND_LABEL wrappers
//      so the CL_VAR for `again: C v;` was never pushed even when
//      cleanups were turned on.
//
// Pattern: g++.dg/init/goto1.C.

int j;

template <class T>
struct C { C() { ++j; } ~C() { --j; } };

int main() {
    {
        int i = 0;
        again:
        C<int> v;
        if (++i < 10) goto again;
    }
    return j;
}
