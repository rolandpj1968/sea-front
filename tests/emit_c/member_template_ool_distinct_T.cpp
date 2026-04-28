// EXPECT: 12
// Two calls of the same Holder<int>::blend with DIFFERENT
// member-template T (deduced int vs deduced double). The two
// instantiations must produce distinct symbols — otherwise their
// dedup keys collide and one is dropped, or worse, the mangled
// names are identical and the C linker conflates them.
//
// Standard: N4659 §17.8.2.1 [temp.deduct.call] (deduction binds
// each member-template parameter from a corresponding call-site
// arg type) + §17.7.1 [temp.inst] (distinct argument sets → distinct
// specializations) + Itanium ABI §5.1 (mangled name encodes the
// deduced types).

template<typename A>
struct Holder {
    A val;
    template<typename T>
    static int blend(Holder<A> *h, T extra);
};

template<typename A>
template<typename T>
int Holder<A>::blend(Holder<A> *h, T extra) {
    return (int)h->val + (int)extra;
}

int main() {
    Holder<int> hi; hi.val = 5;
    int a = Holder<int>::blend(&hi, 4);    // T=int   → 5 + 4 = 9
    int b = Holder<int>::blend(&hi, 2.5);  // T=double → 5 + 2 = 7  (cast trunc)
    return a + b - 4;                       // 9 + 7 - 4 = 12
}
