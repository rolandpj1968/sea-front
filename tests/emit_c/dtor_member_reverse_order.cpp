// EXPECT: 21
// Multiple non-trivial members are destroyed in REVERSE
// declaration order. 'a' is declared first, 'b' second, so
// ~b runs first then ~a.
//
// g sequence: 0 → 0*10+2=2 → 2*10+1=21.
int g = 0;

struct A { ~A() { g = g * 10 + 1; } };
struct B { ~B() { g = g * 10 + 2; } };

struct Outer {
    A a;
    B b;
};

int main() {
    {
        Outer o;
    }
    return g;
}
