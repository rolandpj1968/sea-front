// EXPECT: 4
// std::initializer_list<E> with a class-typed E that has a
// user-declared constructor — each element must run E's ctor,
// not aggregate-init the storage as if it were POD. Sea-front
// allocates auto-storage for the backing array, then issues a
// mangled ctor call per element.
//
// KNOWN GAP: dtor cleanup at scope exit isn't wired through the
// cleanup chain for the synthesised backing array. Tests that
// observe destruction (e.g. g++.dg/cpp0x/initlist6.C's third
// check `if (c != 0) return 3;` after the inner block) still
// fail; the ctor-side progress alone moves them from exit-2 to
// exit-3. The fix is in the broader array-of-class cleanup
// integration.
//
// Pattern: g++.dg/cpp0x/initlist6.C, initlist117.C.

namespace std {
    template<class E>
    class initializer_list {
        const E* _M_array;
        unsigned long _M_len;
    public:
        constexpr initializer_list(const E *a, unsigned long l)
            : _M_array(a), _M_len(l) {}
        constexpr initializer_list() : _M_array(0), _M_len(0) {}
        constexpr const E* begin() const { return _M_array; }
        constexpr unsigned long size() const { return _M_len; }
    };
}

int ctor_calls = 0;
struct A {
    int v;
    A(int x) : v(x) { ++ctor_calls; }
};

int main() {
    // Brace-init element form: each {N} matches A(int) ctor.
    std::initializer_list<A> l { {1}, {2} };
    // 2 ctor calls + 2 elements observed via size() = 4.
    return ctor_calls + (int)l.size();
}
