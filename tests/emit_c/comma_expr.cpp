// EXPECT: 7
// Test: comma expressions emit both sides, result is rhs
int foo() { return 3; }

int main() {
    int x = (foo(), 7);
    return x; // 7
}
