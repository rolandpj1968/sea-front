// EXPECT: 7
// Regression: a free function template whose pattern can't unify with
// the call-site arg (arity mismatch on inner template-id args) must
// be dropped from the candidate set per N4659 §17.8.2 [temp.deduct]/4.
// Previously sema discarded the deduction return value and let an
// un-bindable candidate stay viable, win resolution via fallthrough,
// and then mangle the call with the loser's signature.
//
// Pattern: gcc 4.8 vec.h has gt_pch_nx<T,A>(vec<T,A,vl_embed>*).
// Calls in gengtype-generated code pass vec<X,Y>*. Deduction
// arity-mismatches on the inner template-id args and binds nothing.
//
// Setup: two same-named overloads. The first has a 3-arg vec
// pattern that fails to unify against the call's 2-arg vec arg;
// the second has a 2-arg vec pattern that unifies cleanly. With
// the fix the second overload wins; without the fix the first
// candidate stays viable, ties on weak ranking, and the rewrite-
// to-template-id bails on its unbound third param.

template<typename T, typename A, typename L> struct vec {};
struct vl_embed {};

// First overload — pattern unifies only with vec<*,*,vl_embed>*.
template<typename T, typename A>
int pick(vec<T, A, vl_embed> *p) { (void)p; return 3; }

// Second overload — pattern unifies with any 2-arg vec template-id.
template<typename T, typename A>
int pick(vec<T, A> *p) { (void)p; return 7; }

struct X {};
struct Y {};

int main() {
    vec<X, Y> v;
    return pick(&v);
}
