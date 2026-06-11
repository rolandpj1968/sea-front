// EXPECT: 60
// std::initializer_list<E> as a function parameter — braced-init-list
// at the call site is lowered to C99 compound literals so the
// backing array lives for the duration of the call expression.
// N4659 §11.6.4/5 [dcl.init.list], C99 §6.5.2.5 [compound literals].

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

int sum_il(std::initializer_list<int> l) {
    int s = 0;
    const int *p = l.begin();
    for (unsigned long i = 0; i < l.size(); i++) s += p[i];
    return s;
}

int main() {
    return sum_il({10, 20, 30});
}
