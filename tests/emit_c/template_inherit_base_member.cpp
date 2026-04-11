// EXPECT: 42
template<typename T>
struct base_t {
    T count;
    T get_count() { return count; }
};

template<typename T>
struct derived_t : base_t<T> {
    T extra;
    T total() { return count + extra; }
};

int main() {
    derived_t<int> d;
    d.count = 30;
    d.extra = 12;
    return d.total();
}
