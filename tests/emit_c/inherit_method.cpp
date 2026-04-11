// EXPECT: 42
// Test: calling a method inherited from a base class
struct Base {
    int val;
    int get() { return val; }
};

struct Derived : Base {
    void set(int v) { val = v; }
};

int main() {
    Derived d;
    d.set(42);
    return d.get();
}
