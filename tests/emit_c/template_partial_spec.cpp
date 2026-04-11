// EXPECT: 42
// Partial template specialization — the gcc 4.8 vec.h pattern
struct vl_embed {};
struct va_heap {};

template<typename T, typename A, typename L>
struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    int len;
    T data[8];
    int length() { return len; }
    T operator[](int i) { return data[i]; }
    void push(T val) { data[len] = val; len = len + 1; }
};

int main() {
    vec<int, va_heap, vl_embed> v;
    v.len = 0;
    v.push(10);
    v.push(20);
    v.push(12);
    return v[0] + v[1] + v[2];
}
