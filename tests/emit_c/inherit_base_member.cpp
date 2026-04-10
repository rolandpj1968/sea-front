// EXPECT: 42
struct Base {
    int val;
    int get() { return val; }
};

struct Derived : Base {
    int extra;
};

int main() {
    Derived d;
    d.val = 42;
    return d.get();
}
