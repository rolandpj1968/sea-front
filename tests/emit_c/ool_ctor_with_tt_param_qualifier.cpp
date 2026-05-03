// EXPECT: 42
// Out-of-line constructor of a class template whose qualifier uses
// a template-template parameter (TT-param). Without recognising the
// 'Box<T,A>::Box(...)' shape as a constructor declarator-id at
// parse-type-spec time, the parser treats 'Box<T,A>::Box' as a
// nested-name type (Box being the injected-class-name of Box<T,A>),
// leaves '()' for the declarator as an abstract function with no
// name, and emits a function returning Box<T,A> instead of a ctor.
//
// Standards:
//   N4659 §15.1 [class.ctor]/2 — "When the unqualified-id of a
//   member function declarator names the same class as the qualifier
//   ..., the function so declared is a constructor."
//   N4659 §15.1 [class.ctor]/1 — "Constructors do not have names" —
//   so the ctor's name is NOT registered in the enclosing namespace
//   (otherwise a same-named class type gets shadowed downstream;
//   manifests as parse failures in libstdc++ <complex>).
//
// Pattern from gcc 4.8 hash-table.h:
//   template<T, template<typename> class A>
//   hash_table<T, A>::hash_table() : htab(NULL) { }

template<typename T> struct DefaultAlloc {};

template<typename T,
         template<typename U> class A>
struct Box {
    int v;
    Box();
};

template<typename T,
         template<typename U> class A>
inline
Box<T, A>::Box()
{ v = 42; }

int main() {
    Box<int, DefaultAlloc> b;
    return b.v;
}
