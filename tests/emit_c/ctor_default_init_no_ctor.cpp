// EXPECT: 0
// Negative case: 'Foo a;' for a class WITHOUT any user ctor
// must NOT emit a Foo_ctor call. Foo_ctor doesn't exist, so
// emitting one would fail to link. Default-init for a trivially-
// default-constructible type is just leaving the storage
// uninitialized, exactly like in C.
//
// We then read .v after writing 0 to it (defensive — in case
// the storage held garbage) to demonstrate the program runs to
// completion. Returns 0.
int g = 0;

struct Foo {
    int v;
};

int main() {
    Foo a;
    a.v = 0;
    return a.v + g;
}
