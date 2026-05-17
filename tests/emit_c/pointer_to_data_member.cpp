// EXPECT: 42
// Pointer-to-data-member — N4659 §11.3.3 [dcl.mptr], §8.5 [expr.mptr.oper].
//   'T C::*'   — the type
//   '&C::m'    — produces a pmem value
//   'obj.*p'   — dereference via the pmem
//   'obj->*p'  — dereference via pmem from a pointer
//
// Sea-front lowers pmem to __PTRDIFF_TYPE__ holding the member's
// byte offset within the owning class. &C::m emits as
// '__builtin_offsetof(struct sf__C, m)'; '.*' / '->*' as
// '*(T *)((char *)<base> + pmem)'.

struct Point {
    int x;
    int y;
};

int main() {
    int Point::*px = &Point::x;
    int Point::*py = &Point::y;
    Point p;
    p.x = 35;
    p.y = 7;
    int a = p.*px;
    int b = p.*py;
    return a + b;    // 35 + 7 = 42
}
