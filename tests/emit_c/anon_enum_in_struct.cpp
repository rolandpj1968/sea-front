// EXPECT: 0
// libstdc++'s template type-traits family uses 'enum { __value = N }'
// inside multiple struct specializations as compile-time constants —
// e.g. <bits/cpp_type_traits.h>'s __is_void / __is_integer / etc.
//
// In C++ each enumerator is struct-scoped (A::__value, B::__value).
// In C, anonymous enum members are at NAMESPACE scope, so emitting
// the enums verbatim into N independent C structs causes
// "redeclaration of enumerator '__value'" errors at the C compile.
//
// Sema has already resolved trait-style references (X::__value →
// literal value) at compile time, so the C emit doesn't need these
// constants at runtime. Sea-front skips emitting anonymous enum
// members inside struct bodies — produces empty struct in C.
struct A { enum { __value = 0 }; };
struct B { enum { __value = 1 }; };
struct C { enum { __value = 2 }; };

int main() {
    // Trait-style refs would be resolved by sema; the structs
    // themselves just need to compile.
    A a; B b; C c;
    (void)a; (void)b; (void)c;
    return 0;
}
