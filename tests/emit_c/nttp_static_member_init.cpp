// EXPECT: 42
// Non-type template parameter (NTTP) value substitution into a
// static data-member initializer — N4659 §17.1/4 [temp.param] +
// §17.7.1 [temp.inst]. The libstdc++ <type_traits> 'integral_constant'
// pattern: a class template parameterised on (typename T, T V) with a
// 'static const T value = V;' member.
//
// Two distinct NTTP values are instantiated below. The mangled tag
// must include the literal value (TY_NTTP_VALUE encoding) so each
// gets its own C symbol. Without that, both instantiations would
// collide on the same 'sf__integral_constant_t_int_unknown_te_' and
// the second emission would overwrite the first.
//
// Verified through the qualifier path (Class::value); the
// ND_QUALIFIED static-member rewrite is exercised in
// static_member_qualified.cpp.

template<typename T, T V>
struct integral_constant {
    static const T value = V;
    typedef T value_type;
};

typedef integral_constant<int, 42> answer;
typedef integral_constant<int, 99> not_answer;
typedef integral_constant<bool, true>  yes;
typedef integral_constant<bool, false> no;

int main() {
    int sum = 0;
    if (yes::value) sum += answer::value;        // +42
    if (no::value)  sum += not_answer::value;    // skipped
    return sum;
}
