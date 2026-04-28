// EXPECT: 7
// vec<pattern_d, ...> and vec<pattern_d *, ...> are TWO distinct
// instantiations. The 'iterate' member's third arg is 'T *' —
// for T=pattern_d that's pattern_d*; for T=pattern_d* that's
// pattern_d**. If sea-front's class-instantiation dedup collapses
// pointer-ness, the wrong vec instantiation gets picked at the
// call site and the third-arg type mismatches the def's
// signature — gcc 4.8 build/genopinit then stack-smashes on the
// truncated write. Per task #149.

template<typename T>
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
    Vec<int> vi;     vi.data = vi_storage; vi.n = 1;

    int *vp_storage[1] = {&storage};
    Vec<int *> vp;   vp.data = vp_storage; vp.n = 1;

    int got_i = 0;     vi.iterate(0, &got_i);   // got_i = 3
    int *got_p = 0;    vp.iterate(0, &got_p);   // got_p = &storage

    return got_i + (got_p ? *got_p : 0);   // 3 + 4 = 7
}
