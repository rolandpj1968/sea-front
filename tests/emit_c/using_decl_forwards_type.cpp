// EXPECT: 5
// Using-declaration forwards the underlying type — N4659 §10.3.3
// [namespace.udecl]/4: 'using B::name;' introduces 'name' as a
// synonym in the current scope with the kind it has in B. Without
// forwarding the actual Type, sea-front's parse_type_specifiers
// fallback maps the alias to TY_INT (opaque), so a function
// declaration like 'inline my_t func(...)' inside the namespace
// returns int instead of the struct.
//
// Real-world hit: gcc 14 libstdc++ <cstdlib>'s 'using ::ldiv_t;'
// inside namespace std, followed by 'inline ldiv_t div(long,long)'.
// Without this fix every libcpp/*.cc file emitted these inline
// helpers as 'static inline int div_p_long_long_pe_(...)' which cc
// rejected as "incompatible types when returning struct sf__ldiv_t
// but int was expected" against the matching extern declaration.

typedef struct { long q; } my_t;

namespace ns {
    using ::my_t;
    /* The function below tests the type-forwarding: 'my_t' as the
     * return type must resolve to the underlying struct, not the
     * type-NULL fallback (which would emit 'int'). The function
     * itself is never called — we just need it to parse + compile. */
    inline my_t make_unused(long x) noexcept {
        my_t r; r.q = x; return r;
    }
}

int main() {
    my_t r;
    r.q = 5;
    return (int)r.q;
}
