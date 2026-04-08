// N4659 §6.4.7/1 [class.mem]/6 — complete-class context.
// Inline member function bodies are parsed *as if* they appeared
// after the closing '}' of the class, so members declared LATER in
// the class body are visible to bodies declared EARLIER. We
// implement this by capturing each in-class function body's token
// range during the eager pass and replaying it after the class
// closes, with the class scope on the lookup chain.

// Forward reference to a later non-template member function.
struct A {
    void f() { g(); h(0); }
    void g() {}
    void h(int) {}
};

// Forward reference to a later member type used in 'new T'.
struct B {
    void f() {
        Inner* p = new Inner;
        (void)p;
    }
    struct Inner { int v; };
};

// Forward reference through an out-of-class definition of a member
// of a class template — exercises parse_declarator keeping the
// class scope across the template-id segment of C<T>::.
template<class T> struct C {
    void g();
    void f();
};
template<class T> void C<T>::f() { g(); }
