// EXPECT: 42
template<typename T>
struct Container {
    T data[8];
    int count;
    void add(T v) { data[count] = v; count = count + 1; }
    T get(int i) { return data[i]; }
};

int main() {
    Container<int> c;
    c.count = 0;
    c.add(10);
    c.add(20);
    c.add(12);
    return c.get(0) + c.get(1) + c.get(2);
}
