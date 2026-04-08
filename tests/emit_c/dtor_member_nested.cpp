// EXPECT: 7
// Three-level member chain: A contains B contains C, where C has
// the only user-declared dtor. A and B both pick up has_dtor
// transitively because their members are non-trivially
// destructible. Synthesized A_dtor → B_dtor → C_dtor cascades.
//
// ~C sets g = 7. None of the intermediate classes have a user
// dtor body, so the synthesized wrappers contain only the
// chain call.
int g = 0;

struct C {
    int v;
    ~C() { g = v; }
};

struct B {
    C c;
};

struct A {
    B b;
};

int main() {
    {
        A a;
        a.b.c.v = 7;
    }
    return g;
}
