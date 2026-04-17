// EXPECT: 5
// Test: __builtin_unreachable() parsed as a call, not swallowed
// gcc_assert pattern: (void)((!(cond)) ? (__builtin_unreachable(), 0) : 0)
int safe_div(int a, int b) {
    (void)((!(b != 0)) ? (__builtin_unreachable(), 0) : 0);
    return a / b;
}

int main() {
    return safe_div(15, 3); // 5
}
