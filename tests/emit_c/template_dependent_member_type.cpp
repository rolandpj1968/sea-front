// EXPECT: 42
// Test: member type referencing a template param resolves correctly
// after instantiation. Pattern: 'Inner<T, A> *ptr' inside a
// partial specialization body where A is a template parameter.
// Currently handled by SHORTCUT in clone.c (SubstMap-tag-lookup
// fallback for non-TY_DEPENDENT types).
struct TagA {};
struct TagB {};

template<typename T, typename Tag> struct Inner { T val; };

template<typename T, typename Tag>
struct Outer {
    Inner<T, Tag> *ptr;
    T get() { return ptr ? ptr->val : 0; }
};

int main() {
    Inner<int, TagA> inner;
    inner.val = 42;
    Outer<int, TagA> outer;
    outer.ptr = &inner;
    return outer.get();
}
