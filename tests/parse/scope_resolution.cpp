namespace math {
    int add(int a, int b) { return a + b; }
    int PI = 3;
}

namespace types {
    struct Point { int x; int y; };
}

struct Foo {
    int x;
    static int count;
};

types::Point p;

int main() {
    int a = math::add(1, 2);
    int b = math::PI;
    int c = Foo::count;
    types::Point q;
    q.x = 1;
    return a + b + c + q.x;
}
