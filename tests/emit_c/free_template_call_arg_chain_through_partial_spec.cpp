// EXPECT: 7
// gcc 4.8 vec.h FOR_EACH_VEC_SAFE_ELT pattern, simplified.
//
// The macro expands to:
//   for (i = 0; vec_safe_iterate((V), (i), &(P)); ++(i))
//
// The `V` argument is a chain like
//   parser->unparsed_queues->last().nsdmis
// — pointer-arrow → method-call → field-access → on a vec instantiation.
//
// vec.h has an empty primary `template<class,class,class> struct vec { };`
// and a populated partial specialization `vec<T,A,vl_embed>` carrying
// every method (last, etc.). Sea-front's canonical-tag lookup of `vec`
// returns the empty primary — `last` isn't in its members, so the chain
// stalls and arg[0]'s resolved_type stays NULL. Overload resolution then
// can't deduce template args for the bare-ident free-fn-template call,
// the callee never gets rewritten to ND_TEMPLATE_ID, and codegen emits
// it bare. ~19 link errors against gcc 4.8 cc1plus traced to this exact
// shape.
//
// Fix: when the canonical class_region misses the member, fall back to
// every other declared specialization with the same tag. The partial
// spec's class_region has `last` and the chain resolves through it.
// N4659 §17.8.3.2 [temp.class.spec.match].

struct vl_embed { };
struct vl_ptr   { };
struct va_heap { typedef vl_ptr   default_layout; };
struct va_gc   { typedef vl_embed default_layout; };

// Empty primary — mirrors vec.h:504.
template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct vec { };

// Populated partial spec for vl_embed — mirrors vec.h:549+.
template<typename T, typename A>
struct vec<T, A, vl_embed> {
    T  *data_;
    int len_;
    T &last() { return data_[len_ - 1]; }
};

// Two-overload free function template, deducing T,A from the first arg.
template<typename T, typename A>
bool grab(const vec<T, A, vl_embed> *v, T **ptr) {
    if (v) { *ptr = v->data_; return true; }
    *ptr = 0;
    return false;
}

template<typename T, typename A>
bool grab(const vec<T, A, vl_embed> *v, T *ptr) {
    if (v) { *ptr = *v->data_; return true; }
    *ptr = 0;
    return false;
}

struct entry {
    vec<int, va_gc> *xs;
};

struct parser_t {
    vec<entry, va_gc> *queues;
};

int main() {
    int x = 7;
    vec<int, va_gc> inner = { &x, 1 };
    entry e_arr[1] = { { &inner } };
    vec<entry, va_gc> outer = { e_arr, 1 };
    parser_t parser_storage = { &outer };
    parser_t *parser = &parser_storage;
    int *p = 0;

    // Macro-expanded FOR_EACH shape: chained access through the partial
    // spec's `last()`, then field access, passed bare-ident to grab().
    grab(parser->queues->last().xs, &p);
    return *p;
}
