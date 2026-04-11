// EXPECT: 100
// Test: multiple methods on same class, correct dispatch
struct Counter {
    int n;
    void inc() { n = n + 1; }
    void add(int x) { n = n + x; }
    int get() { return n; }
    void reset() { n = 0; }
};

int main() {
    Counter c;
    c.n = 0;
    c.inc();
    c.add(9);
    c.add(90);
    return c.get();
}
