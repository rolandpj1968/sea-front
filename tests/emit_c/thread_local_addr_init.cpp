// EXPECT: 0
// `T *p = &<tls-var>;` at file scope: the address of a
// _Thread_local variable is NOT a C constant expression (TLS
// storage is resolved at thread init, so the linker can't bake
// the address into a static initialiser).
//
// Pre-fix sea-front treated `&<any-ident>` as a constant
// initializer and emitted `T *p = &tls;` which the C front
// rejected with "initializer element is not constant".
//
// Now the deferral check inspects the resolved_decl's
// is_thread_local flag and routes the init through
// __sf_global_init when the target is TLS.
//
// Pattern: g++.dg/tls/thread_local-order{1,2}.C.

extern "C" void abort();

thread_local int counter;
int *p = &counter;  // must defer to __sf_global_init

int main() {
    counter = 42;
    if (*p != 42) abort();
    return 0;
}
