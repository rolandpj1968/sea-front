// EXPECT: 42
// Compound assignment 'lhs op= rhs' has the same C++17 sequencing as
// plain '=' — N4659 §8.18 [expr.ass]/1: RHS before LHS. The seq-hoist
// in hoist_temps_in_expr fires through the same ND_ASSIGN gate, so
// '*p++ += g()' lowers with rhs bound to '__SF_seq_*' before the
// LHS's '*p++' value computation runs. See docs/sequencing.md.

int side;
int g() { return ++side; }

int main() {
    int arr[3] = {10, 20, 30};
    int *p = arr;
    side = 0;
    *p++ += g();   // C++17: g() runs first (side=1), then arr[0]+=1.
                   // arr = {11, 20, 30}, p points to arr[1].
    return (arr[0] == 11 && p == arr + 1 && side == 1) ? 42 : 1;
}
