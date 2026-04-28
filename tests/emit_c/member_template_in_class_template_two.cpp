// EXPECT: 7
// Closer to gcc 4.8 vec.h: a class template with a static
// member template that takes a pointer-to-class-template type.
// Exercises both heads with non-trivial substitution.
//
// gcc 4.8 hash_table:
//   template<typename Desc>
//   struct hash_table {
//       template<typename T>
//       static T *find_with_hash(hashval_t v);
//   };
//
// Standard: N4659 §17.5.2 [temp.mem] + §17.8.2.5 [temp.deduct.type]
// (deduction unifies through compound types — Holder<A>* against
// the call-site Holder<int>* binds A=int).

template<typename A>
struct Holder {
    A val;
    template<typename T>
    static int sum(Holder<A> *h, T extra) { return (int)h->val + (int)extra; }
};

int main() {
    Holder<int> h;
    h.val = 4;
    return Holder<int>::sum(&h, 3);   // 4 + 3 = 7
}
