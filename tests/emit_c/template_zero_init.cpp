// EXPECT: 0
// Test: vNULL-style zero initialization and struct=0 in template
// bodies. Patterns from gcc vec.h:
//   vec<T,A,vl_ptr> v = vNULL;  (conversion operator → {0})
//   *ptr = 0;  (zero-init for POD types in template body)
struct vnull {
    template<typename T> operator T() { T t = {}; return t; }
};
extern vnull vNULL;
vnull vNULL;

template<typename T>
struct Wrapper {
    T *ptr;
    void clear() { ptr = 0; }
    bool empty() { return ptr == 0; }
};

int main() {
    Wrapper<int> w = vNULL;
    // w.ptr should be 0 from zero-init
    if (!w.empty()) return 1;
    int x = 5;
    w.ptr = &x;
    w.clear();
    return w.empty() ? 0 : 1;
}
