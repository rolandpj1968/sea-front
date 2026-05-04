// EXPECT: 7
// A forward declaration with NO defaults precedes the primary that
// supplies them all. find_primary_template_in_scope must pick the
// later primary as canonical (most named params + most defaults) so
// the merge inherits the defaults from the right declaration.
// Pattern from gcc 4.8 vec.h:
//   template<typename, typename, typename> struct vec;        // forward
//   template<typename T, typename A = va_heap,                // primary
//            typename L = typename A::default_layout>
//   struct vec { ... };
// Sea-front's earlier "first-match wins" path latched onto the
// 0-defaults forward and lost all three defaults. The fixed picker
// scores by named-params and doesn't.

template<class, class, class>
struct Holder;     // forward — three unnamed params, no defaults

template<class T1 = int,
         class T2 = int,
         class T3 = int>
struct Holder {
    T1 a; T2 b; T3 c;
};

int main() {
    Holder<> h = { 2, 2, 3 };
    return h.a + h.b + h.c; // 2 + 2 + 3 = 7
}
