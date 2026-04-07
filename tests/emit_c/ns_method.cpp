// EXPECT: 7
// Method defined inside a namespace — class tag and method name
// must both be mangled with the namespace prefix.
namespace geom {
    struct Point {
        int x;
        int y;
        int sum() { return x + y; }
    };
}

int main() {
    geom::Point p;
    p.x = 3;
    p.y = 4;
    return p.sum();
}
