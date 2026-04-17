// EXPECT: 30
// Test: reference params lowered to pointers — member access uses '->'
struct Pair {
    int a;
    int b;
    Pair() : a(0), b(0) {}
    Pair(int x, int y) : a(x), b(y) {}
    int sum_with(const Pair& other) {
        return a + b + other.a + other.b;
    }
};

int main() {
    Pair p(10, 8);
    Pair q(7, 5);
    return p.sum_with(q); // 10+8+7+5 = 30
}
