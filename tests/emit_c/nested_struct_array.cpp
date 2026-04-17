// EXPECT: 42
// Test: nested struct defined inline as array member gets its body
// emitted before the outer struct (array-of-struct dependency)
struct Outer {
    struct Inner {
        int val;
    } items[4];
    int count;
};

int main() {
    Outer o;
    o.count = 3;
    o.items[0].val = 10;
    o.items[1].val = 20;
    o.items[2].val = 12;
    int sum = 0;
    for (int i = 0; i < o.count; i++)
        sum += o.items[i].val;
    return sum; // 42
}
