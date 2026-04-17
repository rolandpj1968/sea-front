// EXPECT: 42
// Test: sema distinguishes tag (struct) from ordinary (variable/function)
// in expression context. N4659 §6.3.10 [basic.scope.hiding].
struct Pair { int a; int b; };

int main() {
    Pair p;
    p.a = 20;
    p.b = 22;
    int Pair = p.a + p.b;  // variable 'Pair' hides the struct tag
    return Pair; // 42
}
