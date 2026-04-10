// EXPECT: 42
// Three-param template with defaults — gcc vec.h pattern
struct heap {};
struct embed {};
struct gc {};

template<typename T, typename A = heap, typename L = embed>
struct vec {
    T *data;
    int len;
    int cap;
    int size() { return len; }
};

int main() {
    vec<int> v1;
    vec<int, gc> v2;
    vec<int, gc, gc> v3;
    v1.len = 10;
    v2.len = 20;
    v3.len = 12;
    return v1.size() + v2.size() + v3.size();
}
