// EXPECT: 40
// Regression for 38e96fa (sema+codegen: qualified-call typedef
// resolves to underlying class type).
//
// When a qualified call uses a typedef alias for a template
// specialisation as the qualifier ('intbox_t::sized'), the
// instantiated def is mangled through the underlying class type
// (sf__Box_t_int_te___sized_*) but the call-site without the fix
// mangled through the typedef name (sf__intbox_t__sized_*) — they
// diverge and the link fails inside the same TU.
//
// gcc 4.8 reproducer: vec_stack_alloc macro typedefs
//   typedef vec<T, va_stack, vl_embed> stackv
// then calls stackv::embedded_size(N). 24 unresolved
// 'sf__stackv__embedded_size_*' refs in df-scan.o until
// resolved_class_type was added to ND_QUALIFIED so codegen
// dispatches through mangle_class_tag() with the full
// template-arg encoding.

template<typename T>
struct Box {
    static int sized(int n) { return n * (int)sizeof(T); }
};

typedef Box<int> intbox_t;

int main() {
    return intbox_t::sized(10);   // 10 * sizeof(int) = 40
}
