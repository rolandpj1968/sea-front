// EXPECT: 42
// Anonymous union as a class member — C11 6.7.2.1/13 makes the
// union's members directly accessible on the enclosing struct.
// Sea-front emits the nested unnamed union tagless and instance-less
// inside the parent struct so the C compiler applies the same rule.
//
// Test pattern modelled on the bare-access fragment of
// g++.dg/expr/ptrmem4.C.

struct S {
    int a;
    union { int x; };
};

int main() {
    S s;
    s.a = 0;
    s.x = 42;
    return s.x;
}
