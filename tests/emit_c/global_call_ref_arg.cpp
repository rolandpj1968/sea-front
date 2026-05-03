// EXPECT: 0
// '::free(v)' inside a function whose 'v' is a reference parameter
// must dereference: emit 'free(*v)', not 'free(v)'. Without sema
// resolving the single-part '::name' to its global decl,
// emit_arg_for_param's "unknown-callee, ref-param ident" pass-through
// path suppressed the deref — '::free(v)' on a 'T*&' parameter freed
// the address-of-pointer (a stack address) instead of the value, and
// glibc reported "double free or corruption" inside gcc 4.8's
// va_heap::release on every gen-tool startup.
//
// N4659 §6.4.3 [basic.lookup.qual]: '::name' is lookup in the global
// namespace. §8.5.3/4 [dcl.init.ref]: a reference is bound to its
// referent on use; the C lowering needs explicit deref of T**.
extern "C" int printf(const char*, ...);

struct dummy {};
static int g_dummy_int;
static int g_freed_count;

extern "C" void my_free(int *p);

void my_free(int *p) {
    if (p == &g_dummy_int) g_freed_count++;
}

static void release_via_ref(int *&p) {
    ::my_free(p);
    p = (int*)0;
}

int main() {
    int *q = &g_dummy_int;
    release_via_ref(q);
    // q should be NULL, my_free should have been called once on &g_dummy_int.
    return (g_freed_count == 1 && q == (int*)0) ? 0 : 1;
}
