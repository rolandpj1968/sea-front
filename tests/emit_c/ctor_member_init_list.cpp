// EXPECT: 42
// Member-init list: 'Outer() : a(7), b(35) {}' lowers to
// member ctor calls at the top of the synthesized Outer_ctor
// body. Each entry like 'a(7)' becomes 'Inner_ctor(&this->a, 7)'.
//
// The class members are defaultable here (Inner has Inner(int)
// with no default ctor), so user MUST provide them in the
// mem-init list. We're not yet warning about missing entries.
struct Inner {
    int v;
    Inner(int x) { v = x; }
};

struct Outer {
    Inner a;
    Inner b;
    Outer() : a(7), b(35) {}
};

int main() {
    Outer o;
    return o.a.v + o.b.v;
}
