// EXPECT: 30
template<typename T>
struct Box {
    T val;
    T get() { return val; }
};

int main() {
    Box<int> a;
    Box<int> b;
    Box<int> c;
    a.val = 10;
    b.val = 20;
    c.val = a.get() + b.get();
    return c.get();
}
