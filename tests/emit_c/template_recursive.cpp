// EXPECT: 42
// Test: recursive template instantiation — vl_ptr delegates to vl_embed.
// Method calls across template boundary resolve correctly.
struct vl_embed {};
struct vl_ptr {};

template<typename T, typename L> struct vec;

template<typename T>
struct vec<T, vl_embed> {
    int len_;
    T data_[4];
    int length() { return len_; }
};

template<typename T>
struct vec<T, vl_ptr> {
    vec<T, vl_embed> *vec_;
    int length() { return vec_->length(); }
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
        sum += storage.data_[i];
    return sum; // 42
}
