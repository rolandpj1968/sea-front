// EXPECT: 42
// Non-type template parameter (NTTP) value substitution into a
// static data-member initializer — N4659 §17.1/4 [temp.param] +
// §17.7.1 [temp.inst]. The libstdc++ <type_traits> 'integral_constant'
// pattern: a class template parameterised on (typename T, T V) with a
// 'static const T value = V;' member. When the template is
// instantiated with concrete (T, V), the cloned class body must
// substitute V → its concrete value in the initializer.
//
// Single int instantiation here: this test exercises the value-
// substitution mechanism (clone.c morphs ND_IDENT 'V' into ND_NUM
// '42'). Verified through the qualifier path (Class::value) — the
// ND_QUALIFIED rewrite is in static_member_qualified.cpp.
//
// TODO(seafront#nttp-mangling): two distinct NTTP values currently
// collide on the same mangled symbol ('integral_constant<int,42>'
// and 'integral_constant<int,99>' both emit
// 'sf__integral_constant_t_int_unknown_te_'). Encoding the NTTP
// value into the mangled tag is a follow-up slice; until then,
// only one instantiation per (T, NTTP-position) is safe.

template<typename T, T V>
struct integral_constant {
    static const T value = V;
    typedef T value_type;
};

typedef integral_constant<int, 42> answer;

int main() {
    return answer::value;
}
