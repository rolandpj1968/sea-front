// EXPECT: 7
// Test: same template instantiated with different types, both correct
template<typename T>
struct Box {
    T val;
    T get() { return val; }
};

int main() {
    Box<int> bi;
    bi.val = 3;
    Box<int> bj;
    bj.val = 4;
    return bi.get() + bj.get();
}
