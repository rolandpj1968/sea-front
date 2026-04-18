// EXPECT: 0
// Test: dependent qualified name as function call, not declaration.
// Pattern: 'A::method(arg)' where A is a template type parameter.
// N4659 §17.7 [temp.res] — dependent names deferred to instantiation.
// Currently handled by SHORTCUT in parse/stmt.c (dependent-qualified
// name detection). This test verifies the call resolves correctly
// at instantiation time.
struct Alloc {
    template<typename T>
    static void release(T*& p) { p = 0; }
};

template<typename T, typename A>
struct Container {
    T *data;
    void cleanup() {
        if (data) A::release(data);
    }
};

int main() {
    int x = 42;
    Container<int, Alloc> c;
    c.data = &x;
    c.cleanup();
    return c.data == 0 ? 0 : 1;
}
