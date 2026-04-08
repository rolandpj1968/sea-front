// EXPECT: 42
// Out-of-class constructor definition. The class body declares
// 'Foo();' (no body); the body lives at namespace scope as
// 'Foo::Foo() : a(7), b(35) {}'. Both forms must be recognized
// as ctors and resolve to the same Class_ctor symbol so they
// link together.
//
// Pre-fix:
//   - parse_type_specifiers ate 'Foo::Foo' as a qualified
//     type-name, leaving '() : a(7) {}' for parse_declarator.
//     The result was a func-def with no name and no body.
//   - The in-class declaration 'Foo();' was emitted as
//     'Class_Foo' (regular method mangling) instead of
//     'Class_ctor', so even if the body had landed it would
//     have been a different symbol.
//
// Both fixed by detecting 'Class::Class(' pattern in parse_type_
// specifiers and propagating pending_is_constructor through to
// both the var-decl and func-def emission paths.
struct Inner {
    int v;
    Inner(int x) { v = x; }
};

struct Foo {
    Inner a;
    Inner b;
    Foo();              // declaration only
};

Foo::Foo() : a(7), b(35) {}   // definition with mem-init list

int main() {
    Foo f;
    return f.a.v + f.b.v;
}
