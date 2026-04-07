// EXPECT: 25
// 'this->member' and 'return this;' written explicitly. Round-trips
// because codegen names the C parameter 'this' (which is a perfectly
// legal identifier in C) and emits the bare token verbatim — sema
// doesn't need to do anything special.
struct Point {
    int x;
    int y;
    int squared_dist() {
        return this->x * this->x + this->y * this->y;
    }
    Point *self() { return this; }
};

int main() {
    Point p;
    p.x = 3;
    p.y = 4;
    Point *q = p.self();
    return q->squared_dist();
}
