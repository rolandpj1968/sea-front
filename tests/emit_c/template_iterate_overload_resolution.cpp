// EXPECT: 24
// gcc 4.8 vec<T>::iterate has two overloads:
//   bool iterate(unsigned ix, T *p)  const;  // copies element
//   bool iterate(unsigned ix, T **p) const;  // sets *p to internal slot
// At a call 'iterate(i, &p)' where p is T*, &p has type T**, the
// resolver must pick the second (T**) overload — not the first.
// Picking T* would silently miscompile to writing a T-sized value
// into a T*-sized slot. N4659 §16.3 [over.match].
//
// Pattern from gcc 4.8 genopinit's vec<pattern_d>::iterate.

struct Item { int v; };

template<typename T>
struct Vec {
    T data[4];
    int n;

    bool iterate(unsigned ix, T *out) const {
        if (ix >= (unsigned)n) return false;
        *out = data[ix];     // copy
        return true;
    }
    bool iterate(unsigned ix, T **out) const {
        if (ix >= (unsigned)n) return false;
        *out = (T *)&data[ix]; // slot ptr
        return true;
    }
};

int main() {
    Vec<Item> v;
    v.n = 3;
    v.data[0].v = 5;
    v.data[1].v = 8;
    v.data[2].v = 11;

    Item *p;
    int sum = 0;
    for (unsigned i = 0; v.iterate(i, &p); ++i)
        sum += p->v;     // 5 + 8 + 11 = 24
    return sum;
}
