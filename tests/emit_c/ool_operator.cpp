// EXPECT: 30
// Test: out-of-class operator definitions get proper class_type
struct DI {
    int v;
    DI() : v(0) {}
    DI(int x) : v(x) {}
    DI operator+(DI other);
};

DI DI::operator+(DI other) {
    DI r;
    r.v = v + other.v;
    return r;
}

int main() {
    DI a(10);
    DI b(20);
    DI c = a + b;
    return c.v; // 30
}
