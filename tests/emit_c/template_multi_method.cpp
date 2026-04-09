// EXPECT: 15
template<typename T>
struct Accum {
    T total;
    void add(T v) { total = total + v; }
    T result() { return total; }
};

int main() {
    Accum<int> a;
    a.total = 0;
    a.add(5);
    a.add(10);
    return a.result();
}
