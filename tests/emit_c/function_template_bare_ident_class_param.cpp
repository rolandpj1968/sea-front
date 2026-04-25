// EXPECT: 7
// Bare-ident call to a function template whose parameter is a
// class-template specialization — the gcc 4.8 vec.h shape:
//
//   template<typename T> unsigned vec_safe_length(vec<T> *v);
//   ...
//   vec<int> *p;
//   unsigned n = vec_safe_length(p);   // bare ident, T deduced from *vec<T>
//
// This exercises both the bare-ident rewrite (visit_call) AND the
// nested-template deduction (deduce_from_pair recursing through
// TY_PTR → TY_STRUCT → template args).

template<typename T>
struct Holder { T value; };

template<typename T>
T fetch(Holder<T> *h) { return h->value; }

int main() {
    Holder<int> hi;
    hi.value = 7;
    return fetch(&hi);   // T=int deduced from Holder<int>*
}
