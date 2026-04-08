// EXPECT: 7
// Function-style direct initialization 'T t(args)' lowers to
//   struct T t;
//   T_ctor(&t, args);
// The class type's tag drives the mangled name; the first arg
// is &t, the rest are the user args. Single-arg case here.
struct Foo {
    int v;
    Foo(int x) { v = x; }
};

int main() {
    Foo a(7);
    return a.v;
}
