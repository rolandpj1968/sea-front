// EXPECT: 11
// Mini-block for ND_EXPR_STMT: a discarded class-typed call result
// is also a temporary that needs its dtor fired at end-of-full-
// expression. The 'use(make());' line wraps in a mini-block; the
// temp's dtor fires before the next statement.
//
// First statement: use(make()) — make() returns a temp T, use()
// reads its .v (which is 5), and the temp is destroyed before
// control reaches the second statement.
//
// ~T sets g = 6 in this test. Then 'int y = g;' reads g after
// the temp is gone, giving y = 6. Return 5 + 6 = 11.
int g = 0;

struct T {
    int v;
    ~T() { g = v + 1; }
};

T make() {
    T t;
    t.v = 5;
    return t;
}

int use(T t) { return t.v; }

int main() {
    int x = use(make());
    int y = g;
    return x + y;
}
