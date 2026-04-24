// EXPECT: 42
// gcc 4.8 vec.h uses 'vNULL' — a struct with a template conversion
// operator to any vec<T,A,L> — to null-initialize vec objects. Sea-front
// lowers 'vNULL' to '{0}'. That works in an init-declarator
//   vec<T> x = vNULL;           → struct sf__vec x = {0};
// but NOT as the RHS of an assignment
//   data.asan_vec = vNULL;      → data.asan_vec = {0};
// C only accepts '{...}' in initializers, not as rvalues. Emit a
// compound literal '(struct T){0}' in the assignment path instead.
// Pattern: gcc 4.8 cfgexpand.c expand_stack_vars
//   data.asan_vec = vNULL;
//   data.asan_decl_vec = vNULL;

struct Vec {
    int *data;
    int size;
};

struct Holder {
    Vec v;
};

int main() {
    Holder h;
    h.v.size = 999;
    h.v.data = (int*)1;
    Vec src = { (int*)0, 42 };
    h.v = src;                   // plain struct copy
    int x = h.v.size;
    h.v = (Vec){0};              // verify compound-literal form compiles
    return x;
}
