// EXPECT: 0
// 'T()' as an expression — a zero-arg functional cast to a class type
// (N4659 §8.2.3 [expr.type.conv]) is value-initialization. Sea-front
// previously emitted it as a literal call to a function named after
// the class — invalid C. Now lower to a compound literal '(struct
// T){0}'.
//
// Surfaced by gcc 4.8 g++.dg/init/ref4 and the wider 'const T& = T()'
// reference-binding cluster.

struct Box { int v; };

int main() {
    Box b = Box();              // value-init via functional cast
    return b.v;                 // zero-initialized → 0
}
