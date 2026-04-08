// Forward-referenced member type used in earlier method body.
// Inside a class body, names declared later in the body are visible
// from earlier inline method bodies (complete-class context).
// We don't fully implement this; instead the parser accepts unknown
// idents as opaque type-names in template-args and new-expressions.
template<class T> struct uptr { uptr(T*); };
struct C {
    void f() {
        uptr<MR> p{new MR};
    }
    struct MR { };
};
