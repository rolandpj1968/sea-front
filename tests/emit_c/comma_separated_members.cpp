// EXPECT: 30
// Test: comma-separated struct member declarations flattened correctly
struct Rect {
    int x, y;
    int w, h;
};

int main() {
    Rect r;
    r.x = 1; r.y = 2;
    r.w = 10; r.h = 17;
    return r.x + r.y + r.w + r.h; // 30
}
