// EXPECT: 1
// Test: operator== overload
struct Pair {
    int a;
    int b;
    int operator==(Pair other) {
        return a == other.a && b == other.b;
    }
};

int main() {
    Pair x; x.a = 3; x.b = 7;
    Pair y; y.a = 3; y.b = 7;
    return x == y;
}
