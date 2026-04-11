// EXPECT: 42
// Comprehensive gcc 4.8 C++ patterns: is-a.h, vec.h, hash-table.h

// --- is-a.h: tag-based RTTI ---
struct tree_base { int code; };
struct tree_decl : tree_base { int uid; };

template<typename T>
struct is_a_helper {
    static bool test(tree_base *p) { return false; }
};

template<>
struct is_a_helper<tree_decl> {
    static bool test(tree_base *p) { return p->code >= 10; }
};

template<typename T>
bool is_a(tree_base *p) { return is_a_helper<T>::test(p); }

// --- vec.h: 3-param template ---
struct va_heap {};
struct vl_embed {};

template<typename T, typename A = va_heap, typename L = vl_embed>
struct vec {
    T *data;
    int length_;
    int alloc_;
    int length() { return length_; }
    void safe_push(T item) { data[length_] = item; length_ = length_ + 1; }
    T operator[](int ix) { return data[ix]; }
};

// --- hash-table.h: descriptor pattern ---
template<typename T>
struct typed_noop_remove {
    static void remove(T *p) {}
};

template<typename Descriptor>
struct hash_table {
    int size_;
    int size() { return size_; }
};

struct int_hash_desc : typed_noop_remove<int> {
    typedef int value_type;
};

// --- Usage ---
int main() {
    tree_decl d;
    d.code = 15;
    d.uid = 42;
    int result = 0;
    if (is_a<tree_decl>((tree_base*)&d))
        result = result + d.uid;
    int buf[8];
    vec<int> v;
    v.data = buf;
    v.length_ = 0;
    v.safe_push(10);
    v.safe_push(20);
    result = result + v[0] + v[1] + v.length();
    hash_table<int_hash_desc> ht;
    ht.size_ = 3;
    result = result - ht.size() - 29;
    return result;
}
