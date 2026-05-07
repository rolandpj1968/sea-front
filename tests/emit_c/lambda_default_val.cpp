// EXPECT: 42
// Default '[=]' capture — implicit by-value of every odr-used local
// or parameter (§8.1.5.2/7). Closure stores T copies; mutations
// inside the lambda don't propagate to the enclosing function's
// originals (a non-mutable lambda would in fact reject mutation,
// but sea-front doesn't enforce that yet — read-only here is fine).

int main() {
    int x = 7;
    int y = 35;
    auto f = [=]() -> int { return x + y; };
    return f();
}
