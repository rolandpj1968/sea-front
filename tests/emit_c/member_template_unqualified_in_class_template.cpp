// EXPECT: 11
// gcc 4.8 va_heap::reserve calls release(v) — but va_heap is a
// non-template class. The harder variant is a CLASS TEMPLATE whose
// member template body calls a sibling unqualified — needs both
// the class-template instantiation AND the member-template
// instantiation to land on the same Holder<int> tag.

template<typename A>
struct Holder {
    A val;
    template<typename T>
    static int helper(T x) { return (int)x + 2; }
    template<typename T>
    static int driver(Holder<A> *h, T x) {
        return (int)h->val + helper(x);   // unqualified sibling call
    }
};

int main() {
    Holder<int> h; h.val = 5;
    return Holder<int>::driver(&h, 4);   // 5 + (4+2) = 11
}
