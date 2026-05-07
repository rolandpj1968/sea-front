// EXPECT: 40
// Mixed captures: 'x' by value (snapshot), 'y' by reference
// (live alias). The lambda mutates y via the captured pointer
// — calling twice accumulates 2*x onto the original y.
// y starts at 20, +10 +10 = 40.

int main() {
    int x = 10;
    int y = 20;
    auto f = [x, &y]() -> int { y += x; return y; };
    f();
    f();
    return y;
}
