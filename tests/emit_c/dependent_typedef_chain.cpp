// EXPECT: 1
// Dependent typedef chain inside a template class:
//   typedef AllocTraits<Alloc> _Alloc_traits;
//   typedef typename _Alloc_traits::difference_type difference_type;
// resolves _Alloc_traits → AllocTraits<Alloc>, then walks into
// _Alloc_traits to find difference_type. The chain is dependent
// because Alloc isn't bound until instantiation.
//
// Two pieces had to land:
//
// (1) Parse: when the chain qualifier is itself a template-id with
//     dependent template args (TY_STRUCT carrying TY_DEPENDENT in
//     its args, e.g. AllocTraits<Alloc>), build a TY_DEPENDENT
//     whose dep_base points at the underlying type. Substitution
//     recursively substitutes into dep_base then looks up
//     dep_member in the resulting concrete class. Previously the
//     parse only flagged chains where the qualifier was a bare
//     template param.
//
// (2) Sema: typedef class-members were registered with
//     entity = ENTITY_VARIABLE. The functional-cast emit path
//     gates on ENTITY_TYPE; with the miscategorisation,
//     'difference_type(x)' fell through to implicit-this method
//     dispatch and died in die_no_overload. Fix is to register
//     ND_TYPEDEF members with ENTITY_TYPE — they're type-names
//     per N4659 §10.1.3 [dcl.typedef] / §10.1.7.1.
//
// Pattern from gcc 14 libstdc++ basic_string::_S_compare.

template<typename T>
struct AllocTraits {
    typedef long difference_type;
};

template<typename Alloc>
struct basic_string {
    typedef AllocTraits<Alloc> _Alloc_traits;
    typedef typename _Alloc_traits::difference_type difference_type;

    static int _S_compare(unsigned long n1, unsigned long n2) {
        const difference_type d = difference_type(n1 - n2);
        return int(d);
    }
};

int main() {
    return basic_string<char>::_S_compare(2, 1);
}
