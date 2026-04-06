namespace math {
    int add(int a, int b) { return a + b; }
}

namespace types {
    typedef int MyInt;
    struct Point { int x; int y; };
}

using namespace types;

MyInt x = 42;
Point p;

int main() {
    using T = int;
    T result = 10;
    return result + x;
}
