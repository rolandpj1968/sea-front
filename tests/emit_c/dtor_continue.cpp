// EXPECT: 5
// Slice C: continue from inside a loop body with non-trivial
// locals. continue lowers to __SF_CONT(__SF_cleanup_<n>); the
// chain runs the dtor and then chains to __SF_loop_cont_<n>,
// which sits at the bottom of the body block — fall-through
// reaches the for-loop iteration step, exactly like a plain
// continue would.
//
// Loop runs i = 0..3 with ids 1..4. At i==1 we continue, so
// only i=0,2,3 contribute to sum: 0 + 2 + 3 = 5.
struct T {
    int id;
    ~T() {}
};

int f() {
    int sum = 0;
    for (int i = 0; i < 4; i = i + 1) {
        T t;
        t.id = i + 1;
        if (i == 1) continue;
        sum = sum + i;
    }
    return sum;
}

int main() {
    return f();
}
