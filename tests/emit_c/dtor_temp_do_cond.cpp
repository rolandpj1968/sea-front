// EXPECT: 3
// Slice D-cond: temp in a do-while condition. The synth must
// be declared OUTSIDE the do body because the 'while (synth)'
// is past the body's brace pair.
//
// Lowered:
//   int __SF_cond_0;
//   do {
//       <body>
//       { temp + assign + cleanup }
//   } while (__SF_cond_0);
//
// i=1: ++i, then T(1).v < 3 ✓ (g=1)
// i=2: ++i, then T(2).v < 3 ✓ (g=2)
// i=3: ++i, then T(3).v < 3 ✗ (g=3, terminate)
int g = 0;

struct T {
    int v;
    T(int x) { v = x; }
    ~T() { g = g + 1; }
};

int main() {
    int i = 0;
    do {
        i = i + 1;
    } while (T(i).v < 3);
    return g;
}
