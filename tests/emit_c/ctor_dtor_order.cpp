// EXPECT: 123
// Test: constructor with different args, members accessed correctly
struct A {
    int id;
    A(int i) : id(i) {}
    int get() { return id; }
};

int main() {
    A x(1);
    A y(2);
    A z(3);
    return x.get() * 100 + y.get() * 10 + z.get();
}
