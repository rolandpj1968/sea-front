// EXPECT: 35
// Test: same operator with different param types gets distinct mangles.
// fpos-style pattern: operator-(long) vs operator-(const Pos&).
struct Pos {
    int v;
    Pos() : v(0) {}
    Pos(int x) : v(x) {}
    Pos operator-(int off) { Pos r; r.v = v - off; return r; }
    int operator-(const Pos& other) { return v - other.v; }
};

int main() {
    Pos a(20);
    Pos b(3);
    Pos c = a - 2;   // Pos::operator-(int) → Pos(18)
    int d = a - b;   // Pos::operator-(Pos&) → 17
    return c.v + d;  // 18 + 17 = 35? No: c.v=18, d=17 → 35
}
