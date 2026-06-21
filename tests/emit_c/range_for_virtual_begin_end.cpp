// EXPECT: 0
// Range-based for loop where the range expression is a reference to
// a polymorphic base, and the begin/end methods are virtual. The
// dynamic type's begin/end must run (vtable dispatch), not the
// static type's. Without the fix, sea-front emitted direct calls
// to the static type's begin/end — `aa.begin()` always called
// A::begin even when aa was bound to a B with overridden begin.
//
// Real-world hit: g++.dg/cpp0x/range-for15.C
// `for (int x : aa)` where aa is `A &` bound to a `B`.

extern "C" void abort();

unsigned int g = 0;

struct A {
    static int data[1];
    virtual int *begin() { g |= 1; return data; }
    virtual int *end()   { g |= 2; return data; }
};
int A::data[1] = { 0 };

struct B : A {
    static int bdata[1];
    int *begin() { g |= 4; return bdata; }
    int *end()   { g |= 8; return bdata; }
};
int B::bdata[1] = { 0 };

int main() {
    B b;
    A &aa = b;
    g = 0;
    for (int x : aa) { (void)x; }
    /* Range-for evaluates begin(aa) and end(aa); both virtual,
     * both should dispatch to B::begin / B::end via the vtable. */
    if (g != (4 | 8)) abort();
    return 0;
}
