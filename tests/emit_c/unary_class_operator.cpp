// EXPECT: 10
// Unary operator methods on a class type ('-x', '+x', '!x', '~x')
// must dispatch to the class's operator method. ND_UNARY previously
// emitted raw C unary, which fails at the C compiler ('wrong type
// argument to unary minus'). Pattern from gcc 4.8 double-int.c's
// 'cst = -cst' where cst is a double_int. N4659 §16.5 [over.oper].
struct Num {
    int v;
    Num operator-() const { Num r; r.v = -v; return r; }
    Num operator~() const { Num r; r.v = ~v; return r; }
    bool operator!() const { return v == 0; }
};

int main() {
    Num n; n.v = 3;
    Num neg = -n;           // -3
    Num compl_ = ~n;        // ~3 = -4
    bool zero = !n;         // false (0)
    Num z; z.v = 0;
    bool one = !z;          // true (1)
    // -3 + -4 + 0 + 1 = -6, but we want a positive EXPECT.
    return -neg.v + -compl_.v + (zero ? 0 : 0) + (one ? 1 : 0) + 2;
    // -(-3) + -(-4) + 0 + 1 + 2 = 3 + 4 + 1 + 2 = 10
}
