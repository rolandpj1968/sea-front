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

int call_const(const X* p) { return p->at(); }  // must pick const
int call_mut(X* p)         { *p->at() = 7; return *p->at(); }  // non-const

int main() {
    X x;
    x.val = 5;
    int v = call_const(&x);    // 5 (calls const at())
    int w = call_mut(&x);      // 7 (non-const at() writes val, then reads)
    return v + w;              // 12
}
