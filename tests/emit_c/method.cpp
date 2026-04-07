// EXPECT: 7
// First C++ method end-to-end:
//   - struct with in-class method
//   - method body uses unqualified member references (x, y)
//   - call site uses 'p.method()' syntax
//
// The lowering pipeline:
//   1. Sema: visit_ident inside the method body marks 'x'/'y' as
//      implicit_this (their declarations live in the class region).
//   2. Codegen: emits the method as a free function 'Point_sum' with
//      a 'struct Point *this' first parameter, rewrites x/y as
//      this->x / this->y.
//   3. Codegen at the call site: 'p.sum()' becomes 'Point_sum(&p)'.
struct Point {
    int x;
    int y;
    int sum() { return x + y; }
};

int main() {
    Point p;
    p.x = 3;
    p.y = 4;
    return p.sum();
}
