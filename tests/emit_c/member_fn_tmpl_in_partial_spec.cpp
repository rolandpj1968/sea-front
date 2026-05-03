// EXPECT: 42
// Member function template inside a partial specialization, with an
// OOL (out-of-line) definition. Mirrors the gcc 4.8 vec.h pattern:
//
//   template<typename T, typename L>
//   struct Box;                      // primary template (forward)
//
//   template<typename T>
//   struct Box<T, int> {             // partial spec (L = int)
//       T val;
//       template<typename U>
//       int combine(U u);            // member fn template DECL
//   };
//
//   template<typename T>             // outer head: re-specifies T
//   template<typename U>             // inner head: the method's own
//   int Box<T, int>::combine(U u) {  // OOL definition
//       return (int)val + (int)u;
//   }
//
// When called as 'b.combine<int>(2)' on 'Box<int, int> b', BOTH the
// outer T (bound by the class instance to int) AND the inner U (bound
// to int via deduction or explicit) must be substituted in the cloned
// splice's class_type and body.
//
// The bug this guards against: the cloned method's class_type leaks
// the partial-spec's T literally, producing a mangled symbol
// sf__Box_t_T_int_te___combine_t_int_te__p_int_pe_ instead of
// sf__Box_t_int_int_te___combine_t_int_te__p_int_pe_.
//
// Standard: N4659 §17.5.2 [temp.mem]/2 + §17.6.5 [temp.class.spec.mfunc]
// — a member of a class template partial specialization is implicitly
// instantiated using the args of the partial spec's template-id.

template<typename T, typename L>
struct Box;

template<typename T>
struct Box<T, int> {
    T val;
    template<typename U>
    int combine(U u);
};

template<typename T>
template<typename U>
int Box<T, int>::combine(U u) {
    return (int)val + (int)u;
}

int main() {
    Box<int, int> b;
    b.val = 40;
    return b.combine<int>(2);
}
