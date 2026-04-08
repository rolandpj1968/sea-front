// EXPECT: 7
// Default-init: 'Foo a;' (no init, no parens) auto-invokes the
// class's default constructor when one exists. The class type
// is tagged with has_default_ctor at parse time when a user-
// declared zero-arg ctor is found.
//
// Lowered:
//   struct Foo a;
//   Foo_ctor(&a);
//
// Without the fix, the declaration would just leave 'a' as
// uninitialized memory and the body of Foo() would never run,
// so g would stay 0 and the program would return 0.
int g = 0;

struct Foo {
    Foo() { g = 7; }
};

int main() {
    Foo a;
    return g;
}
