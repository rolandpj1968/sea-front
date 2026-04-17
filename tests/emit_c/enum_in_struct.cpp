// EXPECT: 2
// Test: enum defined via typedef then used as struct member doesn't
// get its body emitted twice (dedup across Type copies)
typedef enum { RED = 0, GREEN = 1, BLUE = 2 } Color;

struct Pixel {
    int x;
    int y;
    Color c;
};

int main() {
    Pixel p;
    p.x = 10;
    p.y = 20;
    p.c = BLUE;
    return p.c; // 2
}
