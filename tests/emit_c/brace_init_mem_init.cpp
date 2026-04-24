// EXPECT: 42
// C++11 uniform initialization in mem-initializer-list:
//   mem-initializer-id braced-init-list
// alongside the classic paren form
//   mem-initializer-id ( expression-list(opt) )
// N4659 §15.6.2 [class.base.init].
//
// Two sites need to accept braces:
//   1. parse_func_body's mem-init loop — for out-of-class ctor defs.
//   2. parse_declaration's deferred-body skip — for in-class ctor defs
//      where the body is captured as a token range. The skip must not
//      mistake a mem-init brace-init for the function body's '{'.
//
// Pattern: libstdc++ 13 atomic_base.h / stl_list.h / bitset / sstream
//   : __atomic_flag_base{ _S_init(__i) } { }
//   : _M_to{__to}, _M_goff{-1, -1, -1}, _M_poff{-1, -1, -1} { ... }
//
// Also verifies OOL-ctor mem-init emit: 'int a, b, c;' parses as one
// flat ND_BLOCK containing three ND_VAR_DECLs; the ctor mem-init walk
// must unpack the flat block, otherwise none of the scalar-member
// assignments fire and the ctor body emits empty.

struct Base {
    int a, b, c;
    Base(int x, int y, int z) : a{x}, b{y}, c{z} {}
};

struct Derived {
    int d;
    Derived(int v);
};

Derived::Derived(int v) : d{v + 1} {}

int main() {
    Base b(10, 5, 20);       // a=10, b=5, c=20
    Derived d(6);            // d=7
    return b.a + b.b + b.c + d.d; // 10+5+20+7 = 42
}
