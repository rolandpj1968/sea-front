// EXPECT: 42
// Mixed mem-init list: scalar members and a class member, all
// listed in declaration order via the mem-init list. Verifies
// that emit_ctor_member_inits handles both kinds intermingled
// in a single walk.
//
// Layout:
//   struct Outer {
//       int        n;     // scalar — ': n(7)' → this->n = 7;
//       Inner      i;     // class  — ': i(5)' → Inner_ctor(&this->i, 5);
//       int        k;     // scalar — ': k(30)' → this->k = 30;
//   };
//
// Result: 7 + 5 + 30 = 42.
struct Inner {
    int v;
    Inner(int x) { v = x; }
};

struct Outer {
    int   n;
    Inner i;
    int   k;
    Outer() : n(7), i(5), k(30) {}
};

int main() {
    Outer o;
    return o.n + o.i.v + o.k;
}
