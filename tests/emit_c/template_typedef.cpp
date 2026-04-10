// EXPECT: 42
template<typename T>
struct Container {
    typedef T value_type;
    value_type val;
    value_type get() { return val; }
};

int main() {
    Container<int> c;
    c.val = 42;
    return c.get();
}
