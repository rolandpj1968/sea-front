// EXPECT: 32
// Default '[&]' capture — N4659 §8.1.5.2/7. Every odr-used local
// or parameter of the enclosing fn becomes an implicit by-ref
// capture. Body walks at parse time after the lambda body's
// REGION_BLOCK has popped, so locals declared inside the lambda
// don't reach lookup_unqualified and aren't captured.

int main() {
    int x = 5;
    int y = 10;
    auto f = [&]() -> int { x = x + 1; return x + y; };
    int r = f();        // x=6, returns 16
    return r + x + y;   // 16 + 6 + 10 = 32
}
