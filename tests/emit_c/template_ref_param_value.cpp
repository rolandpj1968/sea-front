// EXPECT: 30
// Test: reference parameter used as a value in assignment and
// function calls within a template body. Pattern from vec.h:
//   void push(const T &obj) { *slot = obj; }
// where obj is T& (lowered to T*) but used as a value.
// Also tests function-pointer calls with ref params:
//   bool (*cmp)(const T&, const T&);  cmp(a, b);
template<typename T>
struct Holder {
    T items[4];
    int count;
    Holder() : count(0) {}
    void push(const T &obj) {
        items[count++] = obj;
    }
    T sum() {
        T s = items[0];
        for (int i = 1; i < count; i++)
            s = s + items[i];
        return s;
    }
};

int main() {
    Holder<int> h;
    int a = 10, b = 20;
    h.push(a);
    h.push(b);
    return h.sum(); // 30
}
