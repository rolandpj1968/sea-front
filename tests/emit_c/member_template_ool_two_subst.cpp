// EXPECT: 30
// Two different class-template instantiations of a class with a
// member template. The OOL def must substitute BOTH heads — A
// from the class template and T from the member template — for
// the cloned body to be correct AND mangle uniquely per
// (Holder<A>, T) pair.

template<typename A>
struct Holder {
    A val;
    template<typename T>
    static int blend(Holder<A> *h, T extra);
};

template<typename A>
template<typename T>
int Holder<A>::blend(Holder<A> *h, T extra) {
    return (int)(h->val * 2) + (int)extra;
}

int main() {
    Holder<int> hi;     hi.val = 5;
    Holder<long> hl;    hl.val = 7;
    int a = Holder<int>::blend(&hi, 6);    // 5*2 + 6 = 16
    int b = Holder<long>::blend(&hl, 0);   // 7*2 + 0 = 14
    return a + b;                           // 30
}
