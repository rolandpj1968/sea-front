// EXPECT: 30
// Test: constructor with mem-initializer-list sets members correctly
struct Rect {
    int w;
    int h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() { return w * h; }
};

int main() {
    Rect r(5, 6);
    return r.area();
}
