// EXPECT: 5
// gcc 4.8 hash-traits pattern: an inner template's body calls
// another class template's static method —
//   pointer_hash<T>::hash(p)
// where T is bound at instantiation time to a concrete type
// (e.g. int). Sema sets resolved_type on the qualified call
// from the class_region lookup, but the lookup's Declaration
// type retains the INNER class template's own parameter name
// ('Type', not the outer T). The outer SubstMap doesn't bind
// 'Type', so subst_type leaves it dependent, and codegen
// mangles the call as '_p_Type_ptr_pe_' instead of the
// substituted '_p_int_ptr_pe_'.
//
// Standard: N4659 §17.7.1 [temp.inst] — instantiation must
// substitute throughout, including in nested template-id calls.
//
// Fix: in the qualified-call codegen path, detect TY_DEPENDENT
// in callee_ft's params and fall through to the TU-walk (which
// matches against the cloned class member's concrete params).

template<typename Type>
struct pointer_hash {
    static int hash(Type *p) { return *p; }
};

template<typename T>
struct hash_user {
    int run(T *p) {
        return pointer_hash<T>::hash(p);
    }
};

int main() {
    int x = 5;
    hash_user<int> h;
    return h.run(&x);
}
