// EXPECT: 42
// Closer reduction of the gcc 4.8 vec.h splice pattern: TWO free outer
// template params plus a fixed sentinel arg.
//
//   template<typename T, typename A, typename L> struct Box;     // primary
//
//   template<typename T, typename A>
//   struct Box<T, A, int> {                  // partial spec, L fixed to int
//       T tval;
//       A aval;
//       template<typename T2, typename A2>
//       int merge(Box<T2, A2, int>& other);
//   };
//
//   template<typename T, typename A>          // outer head
//   template<typename T2, typename A2>        // inner head
//   int Box<T, A, int>::merge(Box<T2, A2, int>& other) {  // OOL definition
//       return (int)tval + (int)aval + (int)other.tval + (int)other.aval;
//   }
//
// vec.h splice has the same shape: 2 free outer params (T, A), 1 fixed
// sentinel (vl_ptr), member fn template with 2 inner params, OOL def.
// The bug it surfaces: cloned merge's class_type leaks the outer T (and
// possibly A) literally, producing a mangled symbol like
// 'sf__Box_t_T_A_int_te___merge_..._pe_' instead of fully substituted.
//
// Standard: N4659 §17.5.2 [temp.mem] + §17.6.5 [temp.class.spec.mfunc]
// + Itanium C++ ABI §5.1.5.

template<typename T, typename A, typename L>
struct Box;

template<typename T, typename A>
struct Box<T, A, int> {
    T tval;
    A aval;
    template<typename T2, typename A2>
    int merge(Box<T2, A2, int>& other);
};

template<typename T, typename A>
template<typename T2, typename A2>
int Box<T, A, int>::merge(Box<T2, A2, int>& other) {
    return (int)tval + (int)aval + (int)other.tval + (int)other.aval;
}

int main() {
    Box<int, int, int> b;
    b.tval = 10;
    b.aval = 11;
    Box<int, int, int> c;
    c.tval = 10;
    c.aval = 11;
    return b.merge(c);
}
