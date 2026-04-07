// EXPECT: 50
// Struct passed and returned by value. C handles this natively, so
// our pipeline just needs to flow 'Point' (the bare class name) through
// param/return positions without losing the struct nature.
struct Point {
    int x;
    int y;
};

int dx(Point a, Point b) {
    return a.x - b.x;
}

Point make() {
    Point p;
    p.x = 100;
    p.y = 0;
    return p;
}

int main() {
    Point a;
    a.x = 50; a.y = 0;
    Point b = make();
    return dx(b, a);
}
