// EXPECT: 42
// Test: cross-template method delegation — one template class
// delegates to another through a pointer member. The inner class's
// methods must be resolvable from the outer's instantiated body.
// Pattern from gcc vec.h: vl_ptr delegates to vl_embed.
// Tests recursive template instantiation + member-type patching.
struct vl_embed {};
struct vl_ptr {};

template<typename T, typename L> struct vec;

template<typename T>
struct vec<T, vl_embed> {
    int len_;
    T data_[4];
    int length() { return len_; }
    T& at(int i) { return data_[i]; }
};

template<typename T>
struct vec<T, vl_ptr> {
    vec<T, vl_embed> *vec_;
    int length() { return vec_ ? vec_->length() : 0; }
    T& at(int i) { return vec_->at(i); }
};

int main() {
    vec<int, vl_embed> storage;
    storage.len_ = 3;
    storage.data_[0] = 10;
    storage.data_[1] = 20;
    storage.data_[2] = 12;

    vec<int, vl_ptr> p;
    p.vec_ = &storage;

    int sum = 0;
    for (int i = 0; i < p.length(); i++)
        sum += p.at(i);
    return sum; // 42
}
