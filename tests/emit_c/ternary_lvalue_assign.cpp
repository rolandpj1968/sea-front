// EXPECT: 42
// Ternary as l-value — N4659 §8.16 [expr.cond]/5.
// '(c ? a : b) = rhs' is a legal C++ assignment when both
// arms are lvalues of the same type. In C the conditional
// is not an lvalue, so sea-front lowers the pattern to
// '*(c ? &a : &b) = rhs'.
//
// Test pattern modelled on gcc 4.8 g++.dg/expr/cond12.

int main() {
    int a = 0;
    int b = 0;
    int which = 1;
    (which ? a : b) = 42;
    return a == 42 && b == 0 ? 42 : 1;
}
