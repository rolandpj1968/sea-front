// EXPECT: 4
// Slice D-cond: temp in a while condition. Each iteration
// constructs a new T(i), tests it, then destroys it BEFORE
// the body runs and BEFORE the next iteration's cond eval.
//
// Lowered: while(1) { mini_block { temp + assign + cleanup } if
// (!synth) break; body; }
//
// i goes 0 → 1 → 2 → 3. T(0).v < 3 ✓ (g=1), T(1).v < 3 ✓ (g=2),
// T(2).v < 3 ✓ (g=3), T(3).v < 3 ✗ (g=4 — fails after one more
// dtor, terminates loop). Total ~T calls = 4.
int g = 0;

struct T {
    int v;
    T(int x) { v = x; }
    ~T() { g = g + 1; }
};

int main() {
    int i = 0;
    while (T(i).v < 3) {
        i = i + 1;
    }
    return g;
}
