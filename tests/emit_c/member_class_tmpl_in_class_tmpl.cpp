// EXPECT: 42
// Member class template inside a class template.
// Outer template: Outer<T>. Inner template: nested class Inner<U>.
// Using Outer<T>::Inner<U> from inside Outer<T>'s methods must
// clone Inner with BOTH outer (T) and inner (U) substitutions.
//
// Standard: N4659 §17.5.2 [temp.mem]/2 — the nested class template
// is instantiated when its specialization is referenced; both the
// enclosing template's args and the nested template's args bind.

template<typename T>
struct Outer {
    T base;

    /* Nested class template. */
    template<typename U>
    struct Inner {
        T outer_val;
        U inner_val;
        int sum() { return (int)outer_val + (int)inner_val; }
    };

    /* Method using the nested class template. */
    int call_inner(int delta) {
        Inner<int> i;
        i.outer_val = base;
        i.inner_val = delta;
        return i.sum();
    }
};

int main() {
    Outer<int> o;
    o.base = 40;
    return o.call_inner(2);
}
