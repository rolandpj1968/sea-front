// EXPECT: 7
// cv-qualifying a class type must yield a DISTINCT Type — not
// retroactively cv-stamp the canonical instance. The
// elaborated-type-specifier-reuse path returns the canonical
// Type to preserve class_region; downstream cv parsing must
// then copy-on-stamp so 'struct T const *' doesn't pollute
// every prior 'struct T *' reference. N4659 §6.9.3
// [basic.type.qualifier] — cv-qualification produces a
// distinct type.
//
// Surfaced by gcc 14's libcpp build: an obstack-macro
// expansion declared 'struct obstack const *__o1' inside a
// statement-expression, which retroactively cv-stamped the
// canonical 'struct obstack' Type and broke every TU-scope
// 'extern void _obstack_newchunk(struct obstack *, ...)'
// declaration (their parameter types became 'const struct
// obstack *', breaking the obstack macro pattern that
// assigns through obstack members).

struct Bag { int v; };

// File-scope extern: parameter is 'struct Bag *' (non-const).
extern int peek(struct Bag *);

// Now introduce 'struct Bag const *' inside a function. Before
// the fix this mutated the canonical Bag Type and made peek's
// param const, breaking 'b->v = 7'.
int touch(struct Bag *b) {
    struct Bag const *bc = b;
    (void)bc;
    b->v = 7;          // would fail if Bag became const-poisoned
    return peek(b);
}

int peek(struct Bag *b) { return b->v; }

int main() {
    Bag b = {0};
    return touch(&b);
}
