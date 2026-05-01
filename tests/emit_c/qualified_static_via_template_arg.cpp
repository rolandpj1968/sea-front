// EXPECT: 5
// Regression: a class template only referenced as a template arg of an
// outer template, with its OOL-defined static method called from the
// outer's substituted body, must still be instantiated. The cloned
// ND_QUALIFIED's leading 'T' is rewritten to the inner class's tag, but
// because lead_tid was NULL on the original 'T::method' parse, the
// class-template collection path has to inspect resolved_class_type
// (set by clone.c) to pick up the inner class for instantiation.
//
// Pattern: gcc 4.8 hash-table.h —
//   hash_table<Descriptor>::find_slot calls Descriptor::hash, with
//   Descriptor bound to e.g. pointer_hash<gimple_statement_d>. Without
//   collecting from resolved_class_type, pointer_hash<gimple_statement_d>
//   never gets instantiated and its OOL hash() function never emits,
//   leaving the call as an undefined reference at link time.
//
// Standard: N4659 §17.7.1 [temp.inst]/1 — implicit instantiation of a
// class template covers any specialization referenced by the program.

struct gimple_d { int x; };

template<typename Type>
struct pointer_hash {
    typedef Type value_type;
    static int hash(const value_type *p);
};

template<typename Type>
int pointer_hash<Type>::hash(const Type *p) { return p->x; }

template<typename T>
struct hash_table {
    int run(const typename T::value_type *p) { return T::hash(p); }
};

int main() {
    hash_table<pointer_hash<gimple_d> > ht;
    gimple_d g; g.x = 5;
    return ht.run(&g);
}
