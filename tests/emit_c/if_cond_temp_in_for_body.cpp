// EXPECT: 3
// If-stmt with class-temp cond as the (unbraced) body of a for.
// emit_c hoists the cond into 'int __SF_cond_N; { ... } if (...)'
// — three statements that must be wrapped in {} so the for-body
// is a single statement. Pattern from gcc 4.8 c-family/c-ada-spec.c.

struct Pair {
    int a;
    int b;
    Pair(int x, int y) : a(x), b(y) {}
    bool both_positive() const { return a > 0 && b > 0; }
};

static Pair make(int n) { return Pair(n, n + 1); }

int main() {
    int count = 0;
    int input[3] = { 1, 2, 3 };
    for (int i = 0; i < 3; i++)
        if (make(input[i]).both_positive())
            count++;
    return count;
}
