// EXPECT: 21
// Slice B: 'return' from inside a block with non-trivial locals
// must still fire their dtors before control leaves the function.
//
// inner() returns void early from a nested block. The return
// rewrite sets __unwind=1 and chains through the cleanup labels:
// inner block runs ~Tracer(b) → g=2, then outer chains to
// ~Tracer(a) → g=21, then __epilogue returns. main observes the
// final g.
int g = 0;

struct Tracer {
    int id;
    ~Tracer() { g = g * 10 + id; }
};

void inner() {
    Tracer a;
    a.id = 1;
    {
        Tracer b;
        b.id = 2;
        return;  // must fire ~b then ~a before actually returning
    }
}

int main() {
    inner();
    return g;
}
