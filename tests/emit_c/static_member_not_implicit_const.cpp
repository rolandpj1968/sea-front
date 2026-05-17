// EXPECT: 6
// A non-const 'static int count;' static data member must lower to
// a MUTABLE TU-scope variable. Previously sea-front emitted it as
// 'static const int sf__C__count;' (inverted-condition bug), so any
// '++count' / 'count = N' in a member function failed cc with
// 'increment of read-only variable'. Surfaced by gcc 4.8
// g++.dg/special/conpr-* and g++.dg/init/dtor3 (9 tests).

struct Counter {
    static int n;
    void bump() { ++n; }
};

int Counter::n = 0;

int main() {
    Counter a, b, c;
    a.bump();
    b.bump();
    c.bump();
    return Counter::n * 2;     // 3 * 2 = 6
}
