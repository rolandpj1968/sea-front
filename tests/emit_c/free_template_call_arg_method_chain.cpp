// EXPECT: 7
// Bare-ident call to an overloaded free function template, where the
// first argument is a method-call → reference → field-access chain on
// a class-template instance:
//
//   grab(outer.last().xs, &p)
//   //   ^^^^^^^^^^^^^^^ vec<entry,va_gc>::last() returns entry&,
//   //                   then .xs is vec<int,va_gc>*
//
// Without substituting the class-template parameters (T=entry,
// A=va_gc) into the looked-up `last()`'s declared return type
// (TY_REF<T>), `outer.last()` ends up with resolved_type = TY_REF<T>
// where T is still TY_DEPENDENT. Then `.xs` strips the ref, sees a
// dependent type instead of `entry`, and silently leaves
// resolved_type=NULL. Overload resolution at the containing call
// then sees a NULL arg-type and refuses to rewrite the callee to
// ND_TEMPLATE_ID, so the free function template never gets
// instantiated — link fails with `U grab` (the bare unmangled
// callee).
//
// This is the gcc 4.8 cp/parser.c FOR_EACH_VEC_SAFE_ELT pattern:
// the macro expands to `vec_safe_iterate(parser->q->last().nsdmis,
// ...)`, which had the same shape and produced ~15 link errors
// (vec_safe_iterate, vec_safe_truncate, vec_safe_push, etc.).
// N4659 §17.5.2 [temp.mem] / §17.6.7 [temp.dep].

struct vl_embed { };
struct va_heap { typedef vl_embed default_layout; };
struct va_gc   { typedef vl_embed default_layout; };

template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct vec {
    T *data;
    int len;
    T &last() { return data[len - 1]; }
};

template<typename T, typename A>
bool grab(const vec<T, A, vl_embed> *v, T **ptr) {
    if (v) { *ptr = v->data; return true; }
    *ptr = 0;
    return false;
}

template<typename T, typename A>
bool grab(const vec<T, A, vl_embed> *v, T *ptr) {
    if (v) { *ptr = *v->data; return true; }
    *ptr = 0;
    return false;
}

struct entry {
    vec<int, va_gc> *xs;
};

int main() {
    int x = 7;
    vec<int, va_gc> inner = { &x, 1 };
    entry e_arr[1] = { { &inner } };
    vec<entry, va_gc> outer = { e_arr, 1 };
    int *p = 0;

    grab(outer.last().xs, &p);
    return *p;
}
