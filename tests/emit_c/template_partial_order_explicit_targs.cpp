// EXPECT: 0
// Function-template partial ordering with explicit template
// arguments — N4659 §17.5.5.2 [temp.func.order] + §17.8.1
// [temp.arg.explicit].
//
// For `f<int>(1)` two templates are viable:
//   template<class T>          T f(int);   // 1 template-param, takes int
//   template<class T, class U> T f(U);     // 2 template-params, takes U
// Both deduce successfully (the first needs nothing from the arg,
// the second deduces U=int). Partial ordering picks the more
// specialized: the first template's param `int` is more specialized
// than the second's `U` (TY_DEPENDENT).
//
// Sea-front previously dropped the first template because
// deduce_template_args was passed an empty SubstMap and required
// `out->nentries > 0` to succeed — the first template has no
// type-deducible param so deduction added zero entries and the
// candidate fell out. Fix: in visit_call's explicit-template-id
// branch, pre-seed the SubstMap with the explicit args. Now the
// map starts with T=int already bound and the candidate passes.
// Pattern: g++.dg/template/spec21.C.

template <class T> T f(int) { return 0; }
template <class T, class U> T f(U) { return 1; }

template <typename T, typename R> T checked_cast(R const &) { return 0; }
template <typename T, typename R> T checked_cast(R *) { return 1; }

int main() {
    int i = 0;
    if (f<int>(1))                       return 1;
    if (checked_cast<int>(i) != 0)       return 2;
    if (checked_cast<int>(&i) != 1)      return 3;
    return 0;
}
