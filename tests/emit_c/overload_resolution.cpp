// EXPECT: 26
// Test: overload resolution picks the right ctor/method by param count
// and type. Point has two sum() overloads and three ctors.
struct Point {
    int x;
    int y;
    Point() : x(0), y(0) {}
    Point(int a, int b) : x(a), y(b) {}
    Point(int v) : x(v), y(v) {}
    int sum() { return x + y; }
    int sum(int extra) { return x + y + extra; }
};

int main() {
    Point a;          // default ctor
    Point b(3, 4);    // 2-arg ctor
    Point c(5);       // 1-arg ctor
    return a.sum() + b.sum() + b.sum(2) + c.sum();
    // 0 + 7 + 9 + 10 = 26? No: 0 + 7 + (3+4+2) + (5+5) = 0+7+9+10 = 26
    // Hmm let me recount: a.sum()=0, b.sum()=7, b.sum(2)=9, c.sum()=10 → 26
}
