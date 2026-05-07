// EXPECT: 42
// By-value captures store T directly in the closure; body
// references emit '__self->name' (no deref). Multiple captures
// stack as members in capture-list order.

int main() {
    int x = 7;
    int y = 35;
    auto f = [x, y]() -> int { return x + y; };
    return f();
}
