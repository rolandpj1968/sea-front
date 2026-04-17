// EXPECT: 42
// Test: method returning T& lowers to T*, 'return *this' → 'return this'
struct Counter {
    int v;
    Counter() : v(0) {}
    Counter& inc() { v++; return *this; }
};

int main() {
    Counter c;
    c.v = 39;
    c.inc();
    c.inc();
    c.inc();
    return c.v; // 42
}
