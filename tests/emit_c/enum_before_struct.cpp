// EXPECT: 1
// Test: enum definitions emitted before struct bodies that use them
enum Direction { UP = 0, DOWN = 1, LEFT = 2, RIGHT = 3 };

struct Movement {
    Direction dir;
    int distance;
};

int main() {
    Movement m;
    m.dir = DOWN;
    m.distance = 5;
    return m.dir; // 1
}
