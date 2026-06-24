// EXPECT: 0
// Fourth stage of resolve-everything-before-emit: sema stamps the
// method overload winner for explicit-member calls `obj.method(args)`
// onto call.resolved_method; codegen reads the stamp instead of
// re-running overload resolution at emit time. N4659 §16.3
// [over.match].

extern "C" void abort();

struct Box {
    int v;
    Box() : v(0) { }
    int compute(int x)         { return v + x; }
    int compute(int x, int y)  { return v + x * y; }
    int compute()              { return v + 100; }
};

int main() {
    Box b;
    b.v = 10;
    /* Three method calls, three overloads — sema picks each winner
     * and codegen mangles via the stamped resolved_method. */
    if (b.compute(5)      != 15)  abort();
    if (b.compute(3, 4)   != 22)  abort();
    if (b.compute()       != 110) abort();
    return 0;
}
