// EXPECT: 9
// OOL definition of a member template inside a class template —
// two template heads. This is the canonical gcc 4.8 pattern from
// vec.h:
//   template<typename T, typename A>
//   template<typename V>
//   void vec<T,A>::quick_push(V x) { ... }
//
// Parser must accept stacked 'template<...>' heads on the OOL
// def. Instantiation pass must find this OOL when the in-class
// member is decl-only.

template<typename A>
struct Holder {
    A val;
    template<typename T>
    static int combine(Holder<A> *h, T extra);  // decl only
};

template<typename A>
template<typename T>
int Holder<A>::combine(Holder<A> *h, T extra) {
    return (int)h->val + (int)extra;
}

int main() {
    Holder<int> h;
    h.val = 4;
    return Holder<int>::combine(&h, 5);   // 4 + 5 = 9
}
