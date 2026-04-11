// EXPECT: 42
typedef unsigned long size_t;
void *malloc(size_t s);
void free(void *p);

struct vl_embed {};
template<typename T, typename A, typename L> struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    int len;
    int alloc;
    T data[16];
    int length() { return len; }
    bool is_empty() { return len == 0; }
    T operator[](int i) { return data[i]; }
    T last() { return data[len - 1]; }
    void quick_push(T val) { data[len] = val; len = len + 1; }
    T pop() { len = len - 1; return data[len]; }
};

struct basic_block { int index; };

template<typename T> struct is_a_helper {
    static bool test(basic_block *) { return false; }
};

int process(vec<basic_block*, int, vl_embed> *wl) {
    int count = 0;
    while (!wl->is_empty()) {
        basic_block *bb = wl->pop();
        count = count + bb->index;
    }
    return count;
}

int main() {
    vec<basic_block*, int, vl_embed> wl;
    wl.len = 0;
    wl.alloc = 16;
    basic_block b1; b1.index = 10;
    basic_block b2; b2.index = 20;
    basic_block b3; b3.index = 12;
    wl.quick_push(&b1);
    wl.quick_push(&b2);
    wl.quick_push(&b3);
    return process(&wl);
}
