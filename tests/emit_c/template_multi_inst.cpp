// EXPECT: 63
template<typename T>
struct Box {
    T val;
    T get() { return val; }
};

template<typename T>
T identity(T x) { return x; }

int main() {
    Box<int> a;
    Box<long> b;
    a.val = 21;
    b.val = 42;
    return identity<int>(a.get()) + (int)b.get();
}
