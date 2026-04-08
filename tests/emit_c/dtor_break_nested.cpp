// EXPECT: 5
// Slice C: a break in an inner loop body must not leak to the
// outer loop. The CL_LOOP marker for the innermost enclosing
// loop is what break_target() returns; the outer loop's marker
// sits below it on the live stack.
//
// Outer loop iterates exactly 5 times; the inner break only
// terminates the inner loop. Returning the outer counter
// observes that the inner break didn't escape.
struct T {
    int id;
    ~T() {}
};

int f() {
    int outer_count = 0;
    for (int i = 0; i < 5; i = i + 1) {
        outer_count = outer_count + 1;
        for (int j = 0; j < 10; j = j + 1) {
            T t;
            t.id = j;
            if (j == 2) break;
        }
    }
    return outer_count;
}

int main() {
    return f();
}
