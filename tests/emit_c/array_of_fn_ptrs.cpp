// EXPECT: 42
// 1D and 2D arrays of function pointers — exercises the
// '(*name[N])(args)' and '(*name[M][N])(args)' grouped declarator
// emit. gcc 4.8 patterns: tree-vect-patterns.c (1D) and i386.c (2D).

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int sub(int a, int b) { return a - b; }

static int (*ops1[3])(int, int) = { add, mul, sub };
static int (*ops2[2][2])(int, int) = {
    { add, mul },
    { sub, add },
};

int main() {
    int x = ops1[0](10, 5);   // 15
    int y = ops1[1](4, 5);    // 20
    int z = ops2[1][0](10, 3); // 7
    return x + y + z;          // 42
}
