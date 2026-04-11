// EXPECT: 35
// Test: operator+ overload produces correct runtime result
struct Vec2 {
    int x;
    int y;
    Vec2 operator+(Vec2 other) {
        Vec2 r;
        r.x = x + other.x;
        r.y = y + other.y;
        return r;
    }
};

int main() {
    Vec2 a;
    a.x = 10; a.y = 20;
    Vec2 b;
    b.x = 3; b.y = 2;
    Vec2 c = a + b;
    return c.x + c.y;
}
