// EXPECT: 42
// Member function template inside a class template.
// Outer template: Outer<T>. Inner template: method template m<U>.
// Calling outer.m<int>(...) inside another method body of Outer<T>
// must clone m with BOTH outer (T) and inner (U) substitutions.
//
// The pattern that surfaced this: gcc 4.8 vec.h has
//   template<typename T, typename A>
//   struct vec<T, A, vl_ptr> {
//     template<typename T2, typename A2>
//     void splice(vec<T2, A2, vl_ptr> &src);
//   };
// and safe_splice() inside the same class calls splice(src). The
// cloned splice's symbol leaked the outer T literally
// ('sf__vec_t_T_va_gc_vl_ptr_te___splice_t_<inner>_te__...') because
// only the inner deduction was in the SubstMap.
//
// Standard: N4659 §17.5.2 [temp.mem] + §17.7.1 [temp.inst].

template<typename T>
struct Outer {
    T base;

    /* Method that calls the member template m<U>. The body's call
     * to m must instantiate against BOTH the outer T (already
     * bound to whatever Outer<T> was instantiated with) AND the
     * inner U (deduced from the call's arg). Pattern matches the
     * gcc 4.8 vec<T,A,vl_ptr>::safe_splice body's call to
     * splice<T2,A2>(src). */
    int call_m(int delta) {
        return m(delta);
    }

    /* Member function template. */
    template<typename U>
    int m(U u) { return base + (int)u; }
};

int main() {
    Outer<int> o;
    o.base = 40;
    return o.call_m(2);
}
