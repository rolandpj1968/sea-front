// EXPECT: 5
// gcc 4.8 hash_table::find body pattern: 'Descriptor::hash(p)'
// where Descriptor is a class-template parameter that, at
// instantiation time, is bound to a class-template instantiation
// like 'pointer_hash<gimple_statement_d>'.
//
// Without this fix: clone.c rewrote parts[0] from the param name
// to the bound type's bare tag ('pointer_hash'), but the
// template_args (gimple_statement_d) were lost. Codegen emitted
// 'sf__pointer_hash__hash_*' (no template args), but the def is
// 'sf__pointer_hash_t_gimple_statement_d_te___hash_*'. Link
// failed with sf__pointer_hash__hash undefineds across cc1plus.
//
// Fix: when cloning ND_QUALIFIED with parts[0] bound to a class-
// template instantiation, set qualified.resolved_class_type so
// codegen mangles through mangle_class_tag (preserves template
// args).
//
// Standard: N4659 §17.7.1 [temp.inst] — instantiation substitutes
// throughout, including the class half of a qualified call.

template<typename T>
struct hasher {
    static int hash(T *p) { return *p; }
};

template<typename Descriptor>
struct user {
    int run(int *p) {
        return Descriptor::hash(p);   // Descriptor → hasher<int>
    }
};

int main() {
    int x = 5;
    user<hasher<int> > u;
    return u.run(&x);
}
