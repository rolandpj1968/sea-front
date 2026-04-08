// EXPECT: 42
// Mem-init list for non-class (scalar) members. Each entry like
// 'x(7)' for an int member lowers to 'this->x = 7;' instead of
// being silently ignored.
//
// Pre-fix the parser captured the entries onto the func node but
// emit_ctor_member_inits only handled TY_STRUCT members, so int /
// pointer / array members were never initialized via the
// mem-init list — the lowered ctor body was empty for a class
// like Point with only int fields.
//
// Lowered:
//   void Point_ctor(struct Point *this, int a, int b) {
//       this->x = a;
//       this->y = b;
//       { /* empty user body */ }
//   }
struct Point {
    int x;
    int y;
    Point(int a, int b) : x(a), y(b) {}
};

int main() {
    Point p(7, 35);
    return p.x + p.y;
}
