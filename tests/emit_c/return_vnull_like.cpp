// EXPECT: 42
// 'return vNULL;' when the enclosing function returns a struct (gcc
// vec.h vec<T,A,L>). vNULL lowers to '{0}', which is valid as an
// init-declarator initializer but NOT as the value of a return
// statement — C only accepts '{...}' in initializers. emit_return_expr
// rewrites this shape to a C99 compound literal '(struct T){0}',
// mirroring the ND_ASSIGN branch that handles 'x = vNULL'.
// Pattern: gcc 4.8 dominance.c get_dominated_by 'return vNULL;'.

struct Vec {
    int *data;
    int size;
};

static Vec empty() { return vNULL; }  // vNULL as return value

struct vnull {
    operator Vec() { Vec r; r.data = 0; r.size = 0; return r; }
};
vnull vNULL;

int main() {
    Vec v = empty();
    if (v.data == 0 && v.size == 0) return 42;
    return 1;
}
