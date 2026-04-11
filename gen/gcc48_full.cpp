typedef unsigned long size_t;
typedef long intptr_t;
extern "C" {
    void *malloc(size_t);
    void *realloc(void *, size_t);
    void free(void *);
    void *memcpy(void *, const void *, size_t);
    void abort();
}
#define NULL 0

// Simplified vec<> matching gcc 4.8 usage patterns
template<typename T, typename A = int, typename L = int>
struct vec {
    T *data_;
    int length_;
    int alloc_;
    int length() { return length_; }
    bool is_empty() { return length_ == 0; }
    T last() { return data_[length_ - 1]; }
    T operator[](int ix) { return data_[ix]; }
    void safe_push(T obj) {
        if (length_ >= alloc_) {
            int na = alloc_ < 4 ? 4 : alloc_ * 2;
            data_ = (T*)realloc(data_, na * sizeof(T));
            alloc_ = na;
        }
        data_[length_] = obj;
        length_ = length_ + 1;
    }
    void truncate(int sz) { length_ = sz; }
    void release() { if (data_) free(data_); data_ = NULL; length_ = 0; alloc_ = 0; }
};

// is-a.h patterns
struct tree_base { int code; };
struct tree_decl : tree_base { int uid; };

template<typename T> struct is_a_helper {
    static bool test(tree_base *) { return false; }
};
template<> struct is_a_helper<tree_decl> {
    static bool test(tree_base *p) { return p->code >= 10; }
};
template<typename T>
bool is_a(tree_base *p) { return is_a_helper<T>::test(p); }

// hash-table.h patterns
template<typename T> struct typed_noop_remove { static void remove(T *) {} };
template<typename Desc> struct hash_table {
    int size_;
    int size() { return size_; }
};
struct int_hash : typed_noop_remove<int> { typedef int value_type; };

// --- Realistic gcc code ---
struct cgraph_node {
    tree_decl *decl;
    int order;
    int uid;
};

int process_nodes(vec<cgraph_node *> worklist) {
    int total = 0;
    while (!worklist.is_empty()) {
        cgraph_node *node = worklist.last();
        worklist.truncate(worklist.length() - 1);
        total = total + node->order;
    }
    return total;
}

vec<int> gather_uids(vec<cgraph_node *> nodes) {
    vec<int> result;
    result.data_ = NULL; result.length_ = 0; result.alloc_ = 0;
    for (int i = 0; i < nodes.length(); i++)
        if (nodes[i]) result.safe_push(nodes[i]->uid);
    return result;
}

bool check_is_decl(tree_base *t) {
    return is_a<tree_decl>(t);
}

int main() {
    // Build nodes
    tree_decl d1; d1.code = 15; d1.uid = 1;
    tree_decl d2; d2.code = 20; d2.uid = 2;
    tree_decl d3; d3.code = 25; d3.uid = 3;
    
    cgraph_node n1; n1.decl = &d1; n1.order = 10; n1.uid = d1.uid;
    cgraph_node n2; n2.decl = &d2; n2.order = 20; n2.uid = d2.uid;
    cgraph_node n3; n3.decl = &d3; n3.order = 12; n3.uid = d3.uid;
    
    // Build worklist
    vec<cgraph_node *> worklist;
    worklist.data_ = NULL; worklist.length_ = 0; worklist.alloc_ = 0;
    worklist.safe_push(&n1);
    worklist.safe_push(&n2);
    worklist.safe_push(&n3);
    
    // Process
    int total = process_nodes(worklist);  // 10+20+12 = 42
    
    // Gather UIDs
    vec<int> uids = gather_uids(worklist);
    int uid_sum = 0;
    for (int i = 0; i < uids.length(); i++)
        uid_sum = uid_sum + uids[i];  // 1+2+3 = 6
    
    // is-a check
    bool ok = check_is_decl((tree_base*)&d1);  // true
    
    // Hash table
    hash_table<int_hash> ht;
    ht.size_ = 6;
    
    // Final: 42 + 6 - 6 + (ok ? 0 : -42) = 42
    int result = total + uid_sum - ht.size();
    if (!ok) result = 0;
    
    uids.release();
    worklist.release();
    return result;
}
