// EXPECT: 12
// Test: function pointer variable declarations emit correctly
// (no spurious '= /* expr */' from stale init field)
int mul(int a, int b) { return a * b; }

int main() {
    int (*fn)(int, int) = &mul;
    return fn(3, 4); // 12
}
