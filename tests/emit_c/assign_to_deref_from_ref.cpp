// EXPECT: 13
// '*slot = obj' where slot is T* and obj is a T reference (lowered
// to T*): the assignment-path adds (*...) around the RHS so the
// struct value is written, but emit_expr(obj) for a ref-param already
// derefs to '(*obj)'. Without suppression these compose to '(*(*obj))'
// which tries to deref a struct value. Pattern: gcc 4.8 vec::quick_push.
struct Pair { int a, b; };

void copy_into(Pair* slot, const Pair& src) {
    *slot = src;
}

int main() {
    Pair p;
    Pair q;
    p.a = 1; p.b = 2;
    q.a = 10; q.b = 3;
    copy_into(&p, q);
    return p.a + p.b;  // 10 + 3 = 13
}
