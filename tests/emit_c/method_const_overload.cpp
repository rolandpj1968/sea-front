// EXPECT: 12
// Const and non-const overloads of the same method. Overload resolution
// must pick the const variant for a const receiver and the non-const
// variant for a non-const receiver. Each overload mangles to a distinct
// symbol (via _const suffix) and the forward declaration's 'this'
// parameter must carry 'const' to match the out-of-line definition.
// N4659 §16.3.1.4 [over.match.funcs]/4.
struct X {
    int val;
    int* at();            // non-const
    int  at() const;      // const
};

int* X::at()       { return &val; }
int  X::at() const { return val; }

int main() {
    X x;
    x.val = 5;
    const X& cx = x;
    int  v = cx.at();      // must pick const overload — cx is const-ref
    int* p = x.at();       // must pick non-const overload
    *p = 7;                // writes x.val through non-const return
    return v + *p;         // 5 (old v) + 7 = 12
}
