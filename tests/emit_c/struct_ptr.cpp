// EXPECT: 15
// Struct passed by pointer + arrow-access. Sema visit_member sees
// p->x: peeks through the pointer to find Point's class_region, then
// resolves x. Codegen emits the '->' verbatim.
struct Point {
    int x;
    int y;
};

int sum(Point *p) {
    return p->x + p->y;
}

int main() {
    Point pt;
    pt.x = 10;
    pt.y = 5;
    return sum(&pt);
}
