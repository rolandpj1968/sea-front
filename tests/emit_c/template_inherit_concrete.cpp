// EXPECT: 42
// Template class inheriting from concrete base — struct layout
struct Base {
    int val;
};

template<typename T>
struct Derived : Base {
    T extra;
};

int main() {
    Derived<int> d;
    d.__sf_base.val = 42;
    d.extra = 0;
    return d.__sf_base.val;
}
