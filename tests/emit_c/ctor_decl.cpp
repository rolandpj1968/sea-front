// EXPECT: 7
// Constructor parsing — first slice. The parser now recognizes
// 'Foo(...)' inside class Foo as a constructor declaration: no
// return type, mangled as Foo_ctor in the lowered C, with the
// usual implicit-this rewrite for member references in the body.
//
// This slice does NOT yet auto-invoke the ctor from a variable
// declaration ('Foo a(7)' is the next slice). main here just
// verifies that:
//   - The ctor declaration is parsed without crashing
//   - The emitted code is valid C and runs to completion
//
// Inspecting gen/emit_c/ctor_decl.c shows the synthesized
// 'void Foo_ctor(struct Foo *this, int x) { (this->v = x); }'.
// Future slices will emit 'Foo_ctor(&a, 7)' next to the
// declaration of 'a'.
struct Foo {
    int v;
    Foo(int x) { v = x; }
};

int main() {
    Foo a;
    a.v = 7;
    return a.v;
}
