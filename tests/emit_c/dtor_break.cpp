// EXPECT: 124
// Slice C: break from inside a loop body with non-trivial locals.
// The loop body has a Tracer whose dtor mutates g; on each
// iteration the dtor fires (via fall-through cleanup chain) and
// on the iteration where break is taken, the same chain runs the
// dtor and then chains to __SF_loop_break_<n> instead of falling
// through to __SF_loop_cont_<n>.
//
// f() runs i=0,1,2 (id 1,2,3). g accumulates: 1, 12, 123.
// At i=2 break is taken — sum is 0+0+1 = 1. main returns 1+123=124.
int g = 0;

struct T {
    int id;
    ~T() { g = g * 10 + id; }
};

int f() {
    int sum = 0;
    for (int i = 0; i < 5; i = i + 1) {
        T t;
        t.id = i + 1;
        if (i == 2) break;
        sum = sum + i;
    }
    return sum;
}

int main() {
    int s = f();
    return s + g;
}
