// EXPECT: 42
// Test: qualified static method call 'Class::method(args)'
// emits as mangled free function without 'this' arg
struct Math {
    static int add(int a, int b) { return a + b; }
    static int mul(int a, int b) { return a * b; }
};

int main() {
    int sum = Math::add(20, 22);
    return sum; // 42
}
