// EXPECT: 7
// Closer to gcc 4.8 vec<T, A, L> shape: three template params,
// last two with defaults. Two instantiations differ only in
// pointer-ness of T:
//   vec<int, va_heap, vl_ptr>     (T=int)
//   vec<int*, va_heap, vl_ptr>    (T=int*)
// Each must produce a distinct instantiation with the correct
// third-arg type for iterate(). If dedup collapses, the vec<int*>
// call sees the vec<int> def — the third arg type mismatches.

struct va_heap {};
struct vl_ptr {};

template<typename T, typename A = va_heap, typename L = vl_ptr>
struct Vec {
    T *data;
    int n;
    bool iterate(int i, T *out) {
        if (i >= n) return false;
        *out = data[i];
        return true;
    }
};

int main() {
    int storage = 4;

    int vi_storage[1] = {3};
    Vec<int> vi;          vi.data = vi_storage; vi.n = 1;

    int *vp_storage[1] = {&storage};
    Vec<int *> vp;        vp.data = vp_storage; vp.n = 1;

    int got_i = 0;        vi.iterate(0, &got_i);
    int *got_p = 0;       vp.iterate(0, &got_p);

    return got_i + (got_p ? *got_p : 0);   // 3 + 4 = 7
}
