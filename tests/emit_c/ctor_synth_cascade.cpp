// EXPECT: 3
// Three-level synthesis cascade: only C has a user-declared
// ctor; B and A both pick up has_default_ctor transitively
// because their members need construction. emit_class_def
// synthesizes B_ctor and A_ctor.
//
// 'A a;' → A_ctor(&a) → B_ctor(&a.b) → C_ctor(&a.b.c) → g = 3.
int g = 0;

struct C { C() { g = g * 10 + 3; } };
struct B { C c; };
struct A { B b; };

int main() {
    A a;
    return g;
}
