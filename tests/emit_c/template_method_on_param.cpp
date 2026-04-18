// EXPECT: 10
// Test: method call on a reference-typed parameter whose type is
// a template instantiation. Pattern: 'other.method()' where other
// is 'const Container<T,A>&' — the Type copy from subst may not
// have class_region, requiring best-effort dispatch.
// Currently handled by SHORTCUT in emit_c.c (template method
// dispatch assumption + arg-type mangling fallback).
template<typename T>
struct Box {
    T val;
    Box() : val(0) {}
    Box(T v) : val(v) {}
    T get() const { return val; }
    bool equals(const Box &other) const {
        return get() == other.get();
    }
};

int main() {
    Box<int> a(10);
    Box<int> b(10);
    if (a.equals(b))
        return a.get();
    return 0;
}
