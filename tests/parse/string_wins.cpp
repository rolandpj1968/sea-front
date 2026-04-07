// Regression tests for blockers fixed while grinding libstdc++ <string>.

// Pointer-to-member declarator: '(_Tp::*pf)()'
template<typename _Ret, typename _Tp> struct mem_fun {
    mem_fun(_Ret (_Tp::*pf)()) : f(pf) {}
    _Ret operator()(_Tp* p) const { return (p->*f)(); }
    _Ret (_Tp::*f)();
};

// '.*' / '->*' as binary operators
struct PMtest {
    int v;
    int run(PMtest* p, int PMtest::* pm) {
        return p->*pm + (*this).*pm;
    }
};

// East-const after qualified type-name in parameter
template<typename _Str>
void str_concat(typename _Str::value_type const* lhs,
                typename _Str::value_type const* rhs) {
    (void)lhs; (void)rhs;
}

// Out-of-class static member definition with template-id qualifier
template<typename T> class Box;
template<typename T> const int Box<T>::npos = -1;

// Constructor inside local struct, with member-init list, opaque type param
template<typename T> void make_guard(int* x) {
    struct Guard {
        int* y;
        explicit Guard(opaque_type* o) : y(0) { (void)o; }
    };
    Guard g(0);
    (void)g; (void)x;
}

// static_cast with unknown destination type
template<typename T> int do_cast(T x) {
    return static_cast<size_type>(x);
}

// extern template explicit instantiation with template-id
template<typename T> class Vec;
extern template class Vec<int>;
template class Vec<char>;
