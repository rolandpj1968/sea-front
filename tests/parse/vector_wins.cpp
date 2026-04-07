// Regression tests for blockers fixed while grinding libstdc++ <vector>.

// Qualified template-id chain in type position: A::B<X>::C
template<typename, typename> struct s { static const bool v = false; };
struct C { typedef int p; };
template<typename T> struct e { typedef T type; };

void qualified_template_id_in_type() {
    e<s<int, C>::v>::type x = 0;
    (void)x;
}

// C-style cast tentative: '(qualified-id)' followed by binary op is NOT a cast
void cast_vs_paren() {
    bool b = (s<int, C>::v) && true;
    (void)b;
}

// Member operator call: x.operator->()
struct M { int* operator->(); };
int member_op_call(M m) {
    return *m.operator->();
}

// Injected class name: bare 'A' inside its own class template body
template<typename T> struct A {
    A(const A&) {}
    template<typename U>
    friend bool operator==(const A&, const A<U>&) { return true; }
};

// Function template overload: free swap and the array swap
template<typename T> void mswap(T& a, T& b) { T t = (a); a = (b); b = (t); }
template<typename T, unsigned long N>
void mswap(T (&a)[N], T (&b)[N]) {
    for (unsigned long i = 0; i < N; ++i) mswap(a[i], b[i]);
}

// Direct-init with two constructor args: 'T x(arg1, arg2);'
struct Pair { Pair(int*, int) {} };
void direct_init_two_args(int* p) {
    Pair x(p, 0);
    (void)x;
}

// Functional cast bool(*ptr) — must NOT eat '*' as abstract pointer decl
struct H { int v; bool eq(const H& o) { return bool(*&v) == bool(o.v); } };

// Inherited member typedef heuristic: unknown 'difference_type' as type
struct Inh { void f(int i) { difference_type n = i; (void)n; } };
