// EXPECT: 7
// Split-decl defaults where a default refers to an EARLIER template
// parameter. N4659 §17.6.4/10 says merging defaults is "in the same
// way default function arguments are" — and §17.6.4/8 admits
// dependent defaults like `class A = typename T::default_layout`.
// The merged head must preserve enough context for substitution to
// resolve the dependent default at instantiation.
//
// Pattern: gcc 4.8 vec.h's primary defaults `L = typename A::
// default_layout` — L's default depends on A. Even when defaults
// are concentrated on one declaration the merge must not break
// dependent-default resolution.

struct vl_ptr   { };
struct va_heap_alike { typedef vl_ptr default_layout; };

template<class, class, class>
struct Slot;     // forward, no defaults

template<class T,
         class A = va_heap_alike,
         class L = typename A::default_layout>
struct Slot {
    T value;
};

int main() {
    Slot<int> s;
    s.value = 7;
    return s.value;
}
