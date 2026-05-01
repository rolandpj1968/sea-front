// EXPECT: 5
// Regression: dependent member-typedef ('typename T::value_type') resolution
// when T is bound to a class-template instantiation whose class_region is not
// yet populated. Mirrors gcc 4.8 hash-table.h's
// hash_table<pointer_hash<gimple_statement_d>>::find_slot pattern.
//
// Standard: N4659 §17.7.1 [temp.inst]/1 — instantiation must resolve dependent
// types throughout the substituted body, including nested-name-specifiers.
//
// Fix: in subst_type, if the substituted T's class_region isn't built but T
// has a template_id_node, walk the template definition's class body for the
// typedef and substitute its RHS against T's template_args.

struct gimple_d;

template<typename Type>
struct pointer_hash {
    typedef Type value_type;
};

template<typename T>
struct hash_table {
    int find(const typename T::value_type *p) { (void)p; return 5; }
};

struct gimple_d { int x; };

int main() {
    hash_table<pointer_hash<gimple_d> > ht;
    gimple_d g; g.x = 0;
    return ht.find(&g);
}
