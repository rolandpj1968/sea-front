// EXPECT: 10
// 'typedef struct X { ... } X;' defines a struct at file scope but
// the parser stores it as ND_TYPEDEF, not ND_CLASS_DEF. The forward-
// decl pass must also emit 'struct X;' for typedef-structs so later
// function parameter lists referencing 'struct X*' land in file scope
// — not in the parameter list's local scope, which would diverge from
// the file-scope struct at the definition site ("conflicting types").
// Pattern from gcc 4.8 combine.c where reg_stat_struct (typedef) is
// the template argument of vec<reg_stat_struct, ...>::iterate whose
// param list references 'struct reg_stat_struct *' before the struct
// body has been emitted.
template<typename T>
struct Holder {
    T* ptr;
    Holder() : ptr(0) {}
    bool fetch(unsigned idx, T* out) const {
        (void)idx; *out = *ptr; return true;
    }
};

typedef struct Item {
    int val;
} Item;

int main() {
    Item x;
    x.val = 10;
    Holder<Item> h;
    h.ptr = &x;
    Item out;
    h.fetch(0, &out);
    return out.val;
}
