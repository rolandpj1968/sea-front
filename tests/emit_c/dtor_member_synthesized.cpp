// EXPECT: 5
// Member dtor synthesis: a class with a non-trivially-destructible
// MEMBER and NO user-declared dtor of its own still needs a
// destructor — to chain into the member's. The synthesized
// Class_dtor wrapper is emitted with just the member calls.
//
// Inner has a non-trivial ~Inner. Outer has a member of type
// Inner and no user dtor. When 'o' goes out of scope, ~Outer
// should fire (synthesized) → ~Inner fires → g becomes 5.
int g = 0;

struct Inner {
    int v;
    ~Inner() { g = v; }
};

struct Outer {
    Inner i;
    /* no user dtor */
};

int main() {
    {
        Outer o;
        o.i.v = 5;
    }
    return g;
}
