// EXPECT: 42
// Brace-init forms for class types — N4659 §11.6.4 [dcl.init.list].
// Both 'T x{args}' (direct-list-init) and 'T x = {args}' (copy-list-
// init) are routed through the same ctor_args path the function-
// style init 'T x(args)' uses. For class types this picks an
// overloaded ctor (we currently assume single overload).
//
// Lowered:
//   struct Foo a; Foo_ctor(&a, 7);
//   struct Foo b; Foo_ctor(&b, 35);
//
// Pre-fix the parser ate both forms with a balanced-brace skip
// loop and the var-decls had no init/ctor data at all.
struct Foo {
    int v;
    Foo(int x) { v = x; }
};

int main() {
    Foo a{7};
    Foo b = {35};
    return a.v + b.v;
}
