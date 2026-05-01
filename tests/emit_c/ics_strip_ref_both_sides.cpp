// EXPECT: 5
// ICS rank for an arg-to-reference-param binding must strip the
// reference from BOTH sides. Sea-front's resolved_type for an
// lvalue arg can carry TY_REF (the param's ref-binding shape);
// stripping only the param side leaves PTR vs REF in
// types_equivalent and ICS_INCOMPATIBLE drops the candidate.
//
// Standard: N4659 §16.3.3.1.4 [over.ics.ref] — the conversion
// sequence to a reference parameter ranks against the referent
// type; whichever side carries the TY_REF, the comparison is
// against the underlying types.
//
// Pattern that surfaces: a templated 'vec_check_alloc<T>' body
// calls 'vec_alloc(v, n)' where v is a 'vec<...>* &' parameter.
// Without this fix the deduced cand fails ICS and the call
// emits unmangled — link error 'undefined reference to vec_alloc'.

template<typename T>
struct vec {};

template<typename T>
int vec_alloc(vec<T> *&v, unsigned n) { (void)v; (void)n; return 5; }

template<typename T>
int wrap(vec<T> *&v) {
    return vec_alloc(v, 0);
}

int main() {
    vec<int> v_storage;
    vec<int> *v = &v_storage;
    return wrap(v);
}
