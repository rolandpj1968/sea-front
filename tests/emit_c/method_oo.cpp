// EXPECT: 7
// Out-of-class method definition. Class declares 'sum()'; the body
// is provided after the class with 'int Point::sum() { return x + y; }'.
// The parser stashes Point's class type on the ND_FUNC_DEF; codegen
// emits it as a mangled free function with the 'this' parameter.
struct Point {
    int x;
    int y;
    int sum();
};

int Point::sum() {
    return x + y;
}

int main() {
    Point p;
    p.x = 3;
    p.y = 4;
    return p.sum();
}
