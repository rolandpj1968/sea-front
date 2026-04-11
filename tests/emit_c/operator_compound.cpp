// EXPECT: 15
// Test: operator+= compound assignment on struct
struct Acc {
    int val;
    void operator+=(int x) { val = val + x; }
};

int main() {
    Acc a;
    a.val = 0;
    a += 5;
    a += 10;
    return a.val;
}
