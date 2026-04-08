// EXPECT: 21
// Slice A of destructor lowering: end-of-block dtor calls fire in
// reverse declaration order (N4659 §15.4 [class.dtor]/8).
//
// 'a' is constructed first then 'b'; at the inner block's '}' the
// dtors fire b-then-a, building up g = ((0*10)+2)*10+1 = 21.
//
// No early exits past non-trivial objects — that's slice B.
int g = 0;

struct Tracer {
    int id;
    ~Tracer() { g = g * 10 + id; }
};

int main() {
    {
        Tracer a;
        a.id = 1;
        Tracer b;
        b.id = 2;
    }
    return g;
}
