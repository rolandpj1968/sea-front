// EXPECT: 42
// 'typedef struct X : Base { ... } *Y;' produces both an ND_CLASS_DEF
// for the struct and an ND_TYPEDEF for the pointer alias. Both decls
// share the tag 'X'. find_class_def_by_tag_only used to treat the
// second match as ambiguous and return NULL — leaving qualified-call
// codegen unable to walk through the struct's bases to resolve an
// inherited static method.
//
// Real-world shape: gcc 4.8 tree-ssa-pre.c
//   typedef struct expr_pred_trans_d
//             : typed_free_remove<expr_pred_trans_d>
//   { ... } *expr_pred_trans_t;
// Then 'hash_table<expr_pred_trans_d>::dispose()' contains
// 'Descriptor::remove(entries[i])' which must resolve to the
// inherited 'typed_free_remove<expr_pred_trans_d>::remove'.
template<typename T>
struct Base {
    static int op(T *p) { (void)p; return 42; }
};

typedef struct Wrapped : Base<Wrapped> {
    int v;
} *WrappedPtr;

template<typename D>
struct User {
    int run() {
        D *p = (D*)0;
        return D::op(p);   // must resolve via Base
    }
};

int main() {
    User<Wrapped> u;
    return u.run();
}
