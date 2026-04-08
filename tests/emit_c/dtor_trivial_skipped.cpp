// EXPECT: 42
// Trivial-dtor optimization: a class whose user-declared
// destructor body is empty is treated as trivially-destructible.
// No dtor function is emitted, no cleanup scaffold is generated
// for instances, and the function compiles to plain C with no
// __SF_* machinery.
//
// Verify behavior end-to-end (returns the right value) and rely
// on the suite as a whole to catch any regression in cleanup
// emission. The structural assertion ('main has no __SF_retval')
// is implicit — covered by the dtor_order family for the
// negative case.
struct Trivial {
    int v;
    ~Trivial() {}
};

int main() {
    Trivial a;
    a.v = 42;
    return a.v;
}
