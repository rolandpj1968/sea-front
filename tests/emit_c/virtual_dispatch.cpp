// EXPECT: 42
// Virtual function dispatch via vtable — N4659 §13.3 [class.virtual].
//
// First slice: single-class polymorphism. The class declares one or
// more virtual methods, gets a per-class vtable struct + static
// instance, a vptr field at offset 0, and call sites dispatch
// through the vptr.
//
// Verified end-to-end: a virtual call through a base pointer
// reaches the right function via the vtable.
//
// First-slice limitation: no derived-class inheritance LAYOUT yet
// (the base's fields aren't yet embedded in the derived struct), so
// this test exercises only single-class virtual dispatch. Cross-
// class polymorphism (Dog : Animal) needs inheritance layout, which
// is its own slice.

struct Box {
    int v;
    Box(int x) : v(x) {}
    virtual int doubled()  { return v + v; }
    virtual int plus(int n) { return v + n; }
    int plain() { return v; }   // non-virtual: not in the vtable
};

int main() {
    Box b(20);
    Box *p = &b;

    int total = 0;

    // Direct virtual call through pointer — dispatches via vtable
    total = total + p->doubled();    // 40

    // Direct virtual call with arg
    total = total + p->plus(1);      // 21 → total = 61

    // Non-virtual call — direct C function call, NOT via vtable
    total = total - p->plain();      // -20 → total = 41

    // Virtual call through value (& receiver lowered the same way)
    total = total + b.plus(0);       // +20 → total = 61

    // Final tally: 40 + 21 - 20 + 20 = 61 - 20 + 20 = 61
    // EXPECT must match — let's recompute:
    //   start  0
    //   +40    40   (doubled)
    //   +21    61   (plus 1)
    //   -20    41   (plain)
    //   +20    61   (plus 0)
    // 61... but we want EXPECT 42 for symmetry.
    // Adjust: subtract 19.
    return total - 19;   // 61 - 19 = 42
}
