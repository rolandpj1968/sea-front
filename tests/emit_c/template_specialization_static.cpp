// EXPECT: 42
// Test: explicit full specialization with static method call.
// Pattern from gcc is-a.h: is_a_helper<Derived>::test(base_ptr)
// N4659 §17.7.3 [temp.expl.spec] — full specialization is a
// concrete declaration, not a template.
struct base { int code; };
struct derived : base { int extra; };

template<typename T> struct is_a_helper {
    static bool test(base *p) { return false; }
};

template<> struct is_a_helper<derived> {
    static bool test(base *p) { return p->code >= 10; }
};

template<typename T>
bool is_a(base *p) { return is_a_helper<T>::test(p); }

int main() {
    derived d;
    d.code = 42;
    d.extra = 7;
    bool ok = is_a<derived>(&d);
    if (ok)
        return d.code;
    return 0;
}
