// EXPECT: 121
// Slice B: multiple returns at different depths in the same
// function. f(3) constructs a (id=1), b (id=2), c (id=1), then
// returns from the deepest block. The goto chain fires ~c
// (g=0*10+1=1), ~b (g=12), ~a (g=121). Each dtor appears
// exactly once in the emitted source.
//
// Ids deliberately chosen so 8-bit exit-code wrap doesn't bite.
int g = 0;

struct T {
    int id;
    ~T() { g = g * 10 + id; }
};

void f(int which) {
    T a; a.id = 1;
    if (which == 1) return;
    {
        T b; b.id = 2;
        if (which == 2) return;
        {
            T c; c.id = 1;
            if (which == 3) return;
        }
    }
}

int main() {
    f(3);
    return g;
}
