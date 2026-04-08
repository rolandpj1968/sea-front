// EXPECT: 0
// Pointer-to-class member is trivially destructible — the
// pointer is just an int-sized scalar. Outer has no user dtor
// and only a pointer member, so Outer itself is trivially
// destructible: no ~Outer is emitted, no cleanup scaffold spins
// up in main, and ~Inner is NEVER called (the pointer doesn't
// own the pointee).
//
// g should stay 0 — Inner's dtor would mutate it if it ran,
// which it must not.
int g = 0;

struct Inner {
    ~Inner() { g = 99; }
};

struct Outer {
    Inner *p;
};

int main() {
    {
        Outer o;
        o.p = (Inner *)0;
    }
    return g;
}
