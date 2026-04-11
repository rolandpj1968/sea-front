// EXPECT: 15
// Test: method access to members via implicit 'this' pointer
struct Point {
    int x;
    int y;
    int sum() { return x + y; }
    void set(int a, int b) { x = a; y = b; }
};

int main() {
    Point p;
    p.set(7, 8);
    return p.sum();
}
