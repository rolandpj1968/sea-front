// EXPECT: 42
// Multi-arg constructor via function-style init. The pre-fix
// parser only captured the FIRST argument and silently dropped
// the rest; the new collector grabs all of them.
//
// Pair p(7, 35) → struct Pair p; Pair_ctor(&p, 7, 35);
// p.a + p.b = 42.
struct Pair {
    int a;
    int b;
    Pair(int x, int y) { a = x; b = y; }
};

int main() {
    Pair p(7, 35);
    return p.a + p.b;
}
