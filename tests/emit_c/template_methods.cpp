// EXPECT: 99
template<typename T>
struct Box {
    T val;
    T get() { return val; }
    void set(T v) { val = v; }
};

int main() {
    Box<int> b;
    b.set(99);
    return b.get();
}
