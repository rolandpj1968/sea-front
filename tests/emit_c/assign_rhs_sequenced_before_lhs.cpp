// EXPECT: 42
// C++17 §8.18 [expr.ass]/1: 'lhs = rhs' sequences RHS before LHS.
// In C the order is unspecified, so naive lowering can mis-execute
// the program when both sides have side effects. Sea-front hoists
// RHS to a fresh '__SF_seq_*' temp at the statement scope so RHS
// is sequenced first by construction (docs/sequencing.md).
//
// Here LHS reads a[i] with i mutated as a side effect on the RHS.
// C++17 picks i=2 (RHS first) so a[2] = 2, leaving a[0] untouched.
// The phase-1 hoist must therefore evaluate '(i = 2)' first.

int main() {
    int i = 0;
    int a[3] = {0, 0, 0};
    a[i] = (i = 2);
    // C++17: a = {0,0,2}, i = 2
    return (a[0] == 0 && a[2] == 2 && i == 2) ? 42 : 1;
}
