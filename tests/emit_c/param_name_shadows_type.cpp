// EXPECT: 99
// Parameter names that happen to coincide with a class/struct type
// name must not leak ctor-state into the enclosing declaration.
// Pattern: 'void bitmap_initialize_stat(bitmap_obstack *obstack)' —
// the param NAME is 'obstack' which ALSO names a struct. Before
// the fix, parse_declarator for the param stashed obstack's
// class_region as qualified_decl_scope and set pending_is_constructor,
// so the enclosing function ended up mangled as
// 'sf__obstack__ctor_...' — conflicting with real callers.
struct obstack { int _marker; };
struct header { int hv; };

// Function whose param name ('obstack') shadows the struct name.
void initialize_with(header* h, obstack *obstack) {
    h->hv = obstack->_marker + 1;
}

// A second free function right after, to surface leakage if any.
int get_hv(header* h) { return h->hv; }

int main() {
    obstack o; o._marker = 98;
    header h; h.hv = 0;
    initialize_with(&h, &o);
    return get_hv(&h);  // 98 + 1 = 99
}
