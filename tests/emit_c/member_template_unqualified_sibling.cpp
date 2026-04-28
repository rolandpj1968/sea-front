// EXPECT: 7
// gcc 4.8 vec.h va_heap::reserve<T> calls release(v) (unqualified)
// — that names the sibling member template va_heap::release<T>.
// Without the fix, the collect path only fires on QUALIFIED
// member-template calls (Class::method form), so the unqualified
// sibling call mangles correctly via the static-this suppression
// fix (#136) but the callee is never instantiated → link fails
// with "undefined reference to sf__A__release_p_int_ptr_ref_pe_".

struct A {
    template<typename T>
    static int release(T x) { return x; }
    template<typename T>
    static int reserve(T x) {
        return release(x) + release(x);   // sibling calls — unqualified
    }
};

int main() {
    return A::reserve(3) + 1;   // 3+3+1 = 7
}
