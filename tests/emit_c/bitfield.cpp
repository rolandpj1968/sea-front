// EXPECT: 7
// Test: bitfield struct members
struct Flags {
    unsigned int a : 3;
    unsigned int b : 4;
    unsigned int c : 1;
};

int main() {
    Flags f;
    f.a = 7;   // 3-bit max = 7
    f.b = 15;  // 4-bit max = 15
    f.c = 1;   // 1-bit max = 1
    return f.a; // 7
}
