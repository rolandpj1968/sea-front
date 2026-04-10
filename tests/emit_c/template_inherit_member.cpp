// EXPECT: 42
struct Base {
    int val;
    int get() { return val; }
};

template<typename T>
struct Derived : Base {
    T extra;
};

int main() {
    Derived<int> d;
    d.val = 42;
    d.extra = 0;
    return d.get();
}
