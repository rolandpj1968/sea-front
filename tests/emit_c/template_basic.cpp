// EXPECT: 42
template<typename T>
struct Box {
    T val;
};

int main() {
    Box<int> b;
    b.val = 42;
    return b.val;
}
