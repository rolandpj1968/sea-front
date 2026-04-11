// EXPECT: 25
// Test: template instantiation produces correct runtime behaviour
template<typename T>
struct Accum {
    T val;
    void add(T x) { val = val + x; }
    T get() { return val; }
};

int main() {
    Accum<int> a;
    a.val = 0;
    a.add(10);
    a.add(15);
    return a.get();
}
