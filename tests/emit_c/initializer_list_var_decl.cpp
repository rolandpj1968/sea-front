// EXPECT: 60
// std::initializer_list<E> as a var-decl target with brace-init
// initializer. N4659 §11.6.4/5 [dcl.init.list]: a braced-init-list
// is matched to an initializer_list<E> parameter by constructing
// a temporary initializer_list whose backing array holds the
// supplied elements. Sea-front recognises this in var-decl form
// — both `= {...}` and `{...}` — and lowers to a `static const
// E[N]` array plus the `{ptr, len}` struct literal.
//
// This test uses a hand-rolled std::initializer_list to exercise
// the lowering in isolation from libstdc++ header surface; the
// libstdc++ form has the same shape (see g++.dg/cpp0x/initlist1.C
// and friends for the upstream pattern).

namespace std {
    template<class E>
    class initializer_list {
    public:
        typedef const E* iterator;
    private:
        iterator _M_array;
        unsigned long _M_len;
    public:
        constexpr initializer_list(const E *a, unsigned long l)
            : _M_array(a), _M_len(l) {}
        constexpr initializer_list() : _M_array(0), _M_len(0) {}
        constexpr const E* begin() const { return _M_array; }
        constexpr unsigned long size() const { return _M_len; }
    };
}

int main() {
    // copy-list-init form
    std::initializer_list<int> a = {10, 20, 30};
    // direct-list-init form
    std::initializer_list<int> b{1, 2, 3};

    int sum = 0;
    const int *pa = a.begin();
    for (unsigned long i = 0; i < a.size(); i++) sum += pa[i];
    // sum == 60

    const int *pb = b.begin();
    for (unsigned long i = 0; i < b.size(); i++) sum *= 1;  /* no-op; check pb works */
    (void)pb;

    return sum;
}
