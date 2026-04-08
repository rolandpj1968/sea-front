// Most-vexing-parse guard — N4659 §9.8/3 [stmt.ambig].
//
// 'g<true, false>()' as a statement is ambiguous between:
//   (a) a function-template call expression, and
//   (b) a declaration with type 'g<true, false>' and an empty
//       abstract declarator '()'.
//
// The standard's "any statement that could be a declaration IS a
// declaration" rule only applies when (b) is *actually* a valid
// declaration. (b) is NOT valid here — there's no declarator-id
// (no name). Our parser must therefore fall back to (a).
//
// Note: this is distinct from the canonical 'S(x);' most-vexing-parse,
// where 'x' IS a valid declarator name and the standard correctly
// resolves to a declaration. We don't override that case.

template<bool, bool> void g();

struct C {
    void f() {
        // Without the no-name guard, this parses as a
        // (var-decl (func-type struct g ())) — a nameless declaration.
        // With the guard, it parses as a call expression.
        g<true, false>();
        g<true, false>(42);
    }
};
