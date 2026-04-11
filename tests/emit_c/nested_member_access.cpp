// EXPECT: 99
// Test: nested struct member access through methods
struct Inner {
    int v;
    int get() { return v; }
};

struct Outer {
    Inner inner;
    int read() { return inner.get(); }
};

int main() {
    Outer o;
    o.inner.v = 99;
    return o.read();
}
