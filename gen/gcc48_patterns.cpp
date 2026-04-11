// Comprehensive gcc 4.8 C++ patterns test
// Exercises the key patterns from vec.h, hash-table.h, is-a.h

typedef unsigned long size_t;
typedef long intptr_t;
extern "C" {
    void *malloc(size_t);
    void *realloc(void *, size_t);
    void free(void *);
    void *memcpy(void *, const void *, size_t);
    void *memset(void *, int, size_t);
    void abort();
}
#define NULL 0

// --- is-a.h patterns: tag-based RTTI without dynamic_cast ---
struct tree_base { int code; };
struct tree_decl : tree_base { int uid; };
struct tree_function_decl : tree_decl { int flags; };

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

// --- vec.h patterns: template class with 3 params + defaults ---
struct va_heap {};
struct vl_embed {};

template<typename T, typename A = va_heap, typename L = vl_embed>
struct vec {
    T *data;
    int length_;
    int alloc_;
    
    int length() { return length_; }
    T last() { return data[length_ - 1]; }
    
    void safe_push(T item) {
        data[length_] = item;
        length_ = length_ + 1;
    }
    
    T operator[](int ix) { return data[ix]; }
};

// --- hash-table.h patterns: descriptor-based hash table ---
template<typename T>
struct typed_noop_remove {
    static void remove(T *p) {}
};

template<typename Descriptor>
struct hash_table {
    typedef typename Descriptor::value_type value_type;
    int size_;
    int size() { return size_; }
};

// Descriptor pattern
struct int_hash_desc : typed_noop_remove<int> {
    typedef int value_type;
    typedef int compare_type;
};

// --- Usage ---
int main() {
    // is-a pattern
    tree_decl d;
    d.code = 15;
    d.uid = 42;
    
    int result = 0;
    if (is_a<tree_decl>((tree_base*)&d))
        result = result + d.uid;
    
    // vec pattern
    int buf[8];
    vec<int> v;
    v.data = buf;
    v.length_ = 0;
    v.alloc_ = 8;
    
    v.safe_push(10);
    v.safe_push(20);
    
    result = result + v[0] + v[1];
    result = result + v.length();
    
    // hash_table pattern
    hash_table<int_hash_desc> ht;
    ht.size_ = 3;
    result = result - ht.size();  // 42 + 10 + 20 + 2 - 3 = 71... 
    
    // let me adjust
    return result - 29;  // = 42
}
