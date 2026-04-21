// EXPECT: 7
// Template parameter used in function-style value-init / conversion:
// 'val(T())' inside a template body, post-instantiation with T=int,
// must emit as 'val((int){0})' — not 'val(T())' as a literal function
// call. Similarly 'val(T(7))' must emit as 'val(((int)(7)))'.
// Clone substitutes the ident's resolved_type but the emitter has to
// rewrite the call shape into a compound literal or cast since
// builtin types have no function symbol.
// N4659 §8.2.3/2 [expr.type.conv].
template<typename T>
struct Holder {
    T val;
    Holder()      : val(T())   {}
    Holder(T v)   : val(T(v))  {}
    T get() const { return val; }
};

int main() {
    Holder<int> a;          // val = int() = 0
    Holder<int> b(7);       // val = int(7) = 7
    return a.get() + b.get();  // 0 + 7 = 7
}
