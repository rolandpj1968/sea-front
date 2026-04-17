// EXPECT: 15
// Test: function returning a function pointer — declarator interleaving
// 'int (*get_adder())(int, int)' not 'int (*)(int,int) get_adder()'
int add(int a, int b) { return a + b; }

typedef int (*binop)(int, int);
binop get_adder() { return &add; }

int main() {
    binop f = get_adder();
    return f(7, 8); // 15
}
