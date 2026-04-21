// EXPECT: 42
// Method call chained on the rvalue result of a function call:
// 'make(v).get()' — the inner call returns a struct by value, which
// is an rvalue in C. Taking '&call()' is invalid C (no lvalue), so
// the codegen must hoist the call to a synthesized temp and use
// '&temp' as the this-arg. Previously only calls returning
// dtor-bearing classes were hoisted; non-dtor structs used as
// method receivers fell through to invalid '&call()'. Pattern from
// gcc 4.8's 'double_int_one.lshift(...)' where double_int_one
// expands to 'double_int::from_shwi(1)'.
struct Box {
    int v;
    int get() const { return v; }
};

Box make(int v) { Box b; b.v = v; return b; }

int main() {
    return make(42).get();
}
