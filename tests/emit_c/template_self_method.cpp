// EXPECT: 42
template<typename T>
struct Counter {
    T count;
    void reset() { count = 0; }
    void add(T n) { count = count + n; }
    T total() { return count; }
    T add_and_get(T n) {
        add(n);
        return total();
    }
};

int main() {
    Counter<int> c;
    c.reset();
    c.add(10);
    c.add(20);
    return c.add_and_get(12);
}
