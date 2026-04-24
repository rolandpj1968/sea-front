// EXPECT: 42
// File-scope anonymous struct with a template-id member:
//   static struct { vec<T> cache; } state;
// The parser hangs the struct body off var_decl.ty->class_def instead
// of producing a separate top-level ND_CLASS_DEF (the tag is synthetic).
// The template-instantiation pass walks tu.decls looking for Types with
// template_id_node set, but if it only follows top-level ND_CLASS_DEFs
// it'll miss templates living inside an anon var-decl's struct body
// and never emit the full definition of vec<T>. Downstream 'by-value
// member of incomplete type' at the C compiler.
//
// Pattern from gcc 4.8 calls.c internal_arg_pointer_exp_state. Limited
// to *anonymous* types: named types already have a top-level class
// def, and descending into them via var-decls causes infinite recursion
// (their methods contain var-decls of their own type).

template<typename T>
struct Box {
    T value;
};

static struct {
    Box<int> cache;
} state;

int main() {
    state.cache.value = 42;
    return state.cache.value;
}
