// EXPECT: 6
// Slice D-MiniBlock: a temporary's dtor must fire at end-of-
// full-expression, not at end of enclosing block. The next
// statement reads g and must see the side effect of ~T.
//
// Concrete observation: ~T increments g. After 'int x = make().v;'
// the temp from make() must already be destroyed, so g == 1 when
// 'int y = g;' runs the next line. Final answer: 5 + 1 = 6.
//
// Without mini-block isolation (extended lifetime), the temp would
// live to end of main and y would read g while it was still 0,
// giving 5 + 0 = 5.
//
// The lowered C splits the var-decl into 'int x;' (outside the
// mini-block) followed by '{ ... x = __SF_temp_0.v; ~temp; }'
// so the dtor runs before y is initialized.
int g = 0;

struct T {
    int v;
    ~T() { g = g + 1; }
};

T make() {
    T t;
    t.v = 5;
    return t;
}

int main() {
    int x = make().v;
    int y = g;
    return x + y;
}
