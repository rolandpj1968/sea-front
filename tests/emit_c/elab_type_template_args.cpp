// EXPECT: 42
// Elaborated-type-specifier with template args ('struct Vec<int>')
// must keep its parsed template_args when reusing the primary
// template's Type. Before the fix, the elab-spec reuse path
// returned the primary's Type bare (whose template_args were the
// template-parameter slots — TY_DEPENDENT placeholders), so a
// local declared as 'struct Vec<int> *x' emitted with the
// primary's 'T' literal in the tag and the call site's mangled
// receiver came out as Vec<TY_DEPENDENT>. Real-world shape: gcc
// 4.8 gt-alias.h auto-generated 'struct vec<alias_set_entry,
// va_gc> * x = ...'. N4659 §17.2/2 + §17.4 [temp.type].
template<typename T>
struct Vec {
    T val;
};

int run(void *x_p) {
    struct Vec<int> *x = (struct Vec<int> *)x_p;
    return x->val;
}

int main() {
    Vec<int> v;
    v.val = 42;
    return run((void *)&v);
}
