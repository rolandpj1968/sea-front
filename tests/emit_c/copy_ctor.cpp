// EXPECT: 77
// Test: copy ctor 'T x(*this)' resolves to T(const T&), not T(int)
struct Box {
    int v;
    Box() : v(0) {}
    Box(int x) : v(x) {}
    Box(const Box& o) : v(o.v) {}
    Box& operator+=(int x) { v += x; return *this; }
    Box add(int x) {
        Box copy(*this);   // must pick copy ctor, not int ctor
        copy += x;
        return copy;
    }
};

int main() {
    Box b(70);
    Box c = b.add(7);
    return c.v; // 77
}
