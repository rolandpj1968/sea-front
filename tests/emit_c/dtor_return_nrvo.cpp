// EXPECT: 6
// Slice D-Return: 'return local;' where 'local' is a class with
// non-trivial dtor is treated as a move (NRVO-style). The local
// IS the return value; its lifetime now belongs to the caller's
// receiving variable. So we must NOT fire the local's dtor in
// the callee.
//
// make() returns t. With D-Return, ~t is skipped inside make.
// In main, T x = make() stores the moved value in x. ~x fires
// when x goes out of scope at the end of the inner block.
// g is incremented exactly once: 1.
// Return v + g = 5 + 1 = 6.
//
// Without D-Return this would have fired ~t in make as well,
// giving g = 2 and a return of 7 — observably double-destroying
// the same object.
int g = 0;

struct T {
    int v;
    ~T() { g = g + 1; }
};

T make() {
    T t;
    t.v = 5;
    return t;
}

int main() {
    int v;
    {
        T x = make();
        v = x.v;
    }
    return v + g;
}
