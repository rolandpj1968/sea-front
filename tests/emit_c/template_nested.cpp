// EXPECT: 42
template<typename T>
struct Box {
    T val;
    T get() { return val; }
};

template<typename T>
struct Outer {
    Box<T> inner;
};

int main() {
    Outer<int> o;
    o.inner.val = 42;
    return o.inner.get();
}
