// EXPECT: 121
// Per-var cleanup labels: a return between two dtor-bearing locals
// in the same block must NOT call the dtor for the not-yet-
// constructed object.
//
// f(1): return after constructing a only → ~a fires → g=1
// f(2): return after constructing a, b   → ~b ~a fires → g=21
//
// Encode both observations in main: 1*100 + 21 = 121.
//
// Pre-fix (Slice B): all dtors in a block lived in one cleanup
// label, so f(1) would have called T_dtor(&b) on uninitialized
// stack — silently corrupting g and possibly crashing.
int g = 0;

struct T {
    int id;
    ~T() { g = g * 10 + id; }
};

void f(int which) {
    T a; a.id = 1;
    if (which == 1) return;
    T b; b.id = 2;
    if (which == 2) return;
    T c; c.id = 3;
}

int main() {
    g = 0; f(1);
    int x = g;
    g = 0; f(2);
    int y = g;
    return x * 100 + y;
}
