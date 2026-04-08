// EXPECT: 12
// Member-init list ordering: members are ALWAYS constructed in
// DECLARATION order (N4659 §15.6.2/13.3), regardless of the
// order in the mem-init list. This is a famous C++ gotcha and
// the reason GCC emits -Wreorder.
//
// Source has 'Outer() : b(2), a(1) {}' but a is declared first
// in the class body, so a's ctor must run before b's.
//
// Inner's ctor records into g via decimal-shift, so the order
// is observable: a (id=1) → g=1, then b (id=2) → g=12.
//
// Without the reordering rule, we'd get g=21 here.
int g = 0;

struct Inner {
    int v;
    Inner(int x) { v = x; g = g * 10 + x; }
};

struct Outer {
    Inner a;
    Inner b;
    Outer() : b(2), a(1) {}
};

int main() {
    Outer o;
    return g;
}
