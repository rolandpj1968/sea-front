// EXPECT: 1
// Reproducer for the literal-A bug found in gcc 4.8 vec.h
// instantiation. The ROOT CAUSE: when parsing an OOL definition
// `template<typename T, typename A> vec<T, A, vl_ptr>::splice(...)`,
// if there's a COMPETING specialization `vec<T, va_gc, vl_ptr>`
// in scope, the parser mis-resolves `A` in the class qualifier
// against the competing spec instead of treating it as the OOL's
// template parameter. The param type's `A` then falls back to
// TY_STRUCT(tag="A") (parser fallback for unknown name) and clone
// substitution leaves it unsubstituted — emitting symbols with
// literal `A` that go undefined at link time.
//
// gcc 4.8 vec.h has both
//   template<typename T, typename A> struct vec<T, A, vl_ptr> { ... };
//   template<typename T>             struct vec<T, va_gc, vl_ptr> {};
// The second is the trigger.

struct va_heap { template<typename T> static void z(T*&){} };
struct va_gc   { template<typename T> static void z(T*&){} };
struct vl_ptr {};
struct vl_embed {};

template<typename T, typename A = va_heap, typename L = vl_ptr>
struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> { int len; };

// PRIMARY-FOR-vl_ptr partial spec (T,A both variable)
template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    vec<T, A, vl_embed> *vec_;
    void splice(vec<T, A, vl_ptr> &);
};

// COMPETING specialization that pins A=va_gc
template<typename T>
struct vec<T, va_gc, vl_ptr> {};

// OOL def for the GENERIC vl_ptr partial spec
template<typename T, typename A>
inline void
vec<T, A, vl_ptr>::splice(vec<T, A, vl_ptr> &src) {
    vec_ = src.vec_;
}

struct gimple {};

int main() {
    vec<gimple, va_heap, vl_ptr> a, b;
    a.vec_ = 0; b.vec_ = 0;
    a.splice(b);
    return 1;
}
