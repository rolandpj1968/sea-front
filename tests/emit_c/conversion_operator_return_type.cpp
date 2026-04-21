// EXPECT: 42
// A conversion function (§16.3.2 [class.conv.fct]) — 'operator T() const'
// — has NO return type in the decl-specifier position; the conversion-
// type-id IS the return type. Parser previously discarded that type,
// built the function with void return, then tripped at 'return
// this->member;' (clang: 'void function should not return a value').
// Pattern from libstdc++ fpos / streamoff. We don't invoke the
// conversion operator implicitly here (that's a separate gap); the
// check is that the emitted C definition has the correct return type
// and its body compiles.
struct Fpos {
    long _M_off;
    Fpos(long o) : _M_off(o) {}
    operator long() const { return _M_off; }
};

int main() {
    Fpos p(42);
    // Sea-front doesn't implicitly invoke conversion operators at
    // init/assignment sites yet — so we don't exercise 'long x = p'
    // here. The test is that the emitted C for 'operator long() const'
    // uses 'long' as the return type rather than void; with 'void'
    // the body's 'return this->_M_off;' fails the downstream compile.
    return (int)p._M_off;
}
