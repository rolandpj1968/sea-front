// EXPECT: 11
// gcc 4.8 vec.h pattern:
//   template<typename T, typename A>
//   void vec<T,A>::reserve(unsigned n) {
//       A::reserve(this, n);   // calls A's member template
//   }
// The CALL site is inside a cloned template body. Without
// member-template-of-non-template-class registry support, this
// already works; we add: ensure the call still resolves through
// the registry when reached during Phase-2 re-collection.
//
// Standard: N4659 §17.7.1 [temp.inst] (an instantiation of a
// function template that calls a member template forces that
// member template's instantiation transitively) + §17.5.2.

struct Allocator {
    template<typename T>
    static int store(T *out, int v) { *out = (T)v; return v; }
};

template<typename T>
struct Vec {
    T data;
    int put(int v) { return Allocator::store(&data, v); }
};

int main() {
    Vec<int> vi;
    int a = vi.put(5);
    Vec<long> vl;
    int b = vl.put(6);
    return a + b;   // 5 + 6 = 11
}
