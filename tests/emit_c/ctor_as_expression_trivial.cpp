// EXPECT: 7
// Constructor-as-expression for a trivially-destructible class.
// 'Trivial(7)' is hoisted to a synthesized local even though
// Trivial has no dtor — there's no symbol named 'Trivial' in
// the lowered C, only 'Trivial_ctor', so source-form emission
// would fail to compile.
//
// Lowered:
//   struct Trivial __SF_temp_0;
//   Trivial_ctor(&__SF_temp_0, 7);
//   return use(__SF_temp_0);
//
// No cleanup chain, no __SF_PROLOGUE — the function doesn't
// need any cleanup machinery because there's nothing to destroy.
// hoist_emit_decl emits the temp local but skips the CL_VAR
// push when the type has no dtor.
//
// Pre-fix the hoist trigger gated on has_dtor, so this would
// fall through to source-form 'use(Trivial(7))' which doesn't
// link.
struct Trivial {
    int v;
    Trivial(int x) { v = x; }
    /* trivially-destructible — no user dtor, no class members */
};

int use(Trivial t) { return t.v; }

int main() {
    return use(Trivial(7));
}
