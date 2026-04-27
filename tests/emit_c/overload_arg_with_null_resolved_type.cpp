// EXPECT: 1
// ics_rank previously rejected EVERY candidate when an arg's
// resolved_type was NULL — sema's arithmetic-expression typing
// has gaps, so a call's later argument can come through
// unresolved even when the earlier args are typed correctly.
// All-rejected → resolve_free_function_overload returns NULL →
// the parser's initial resolved_decl wins (whichever overload
// happened to be registered first), defeating overload resolution.
//
// Concrete: gcc 4.8 reginfo.c record_subregs_of_mode calls
//   bitmap_set_bit(subregs_of_mode, regno*NUM_MACHINE_MODES+(unsigned)mode)
// where subregs_of_mode is bitmap (struct bitmap_head_def *) but
// the second arg's resolved_type was NULL. Both bitmap_set_bit
// overloads (sbitmap-version returns void, bitmap-version returns
// bool) got rejected; the parser had registered the sbitmap one
// first; codegen mangled to it; the C compiler errored
// 'void value not ignored as it ought to be' on the if (...) wrap.
//
// Fix: when ics_rank sees arg=NULL, treat as wildcard ICS_EXACT
// for that slot. The OTHER arg's ICS scores still discriminate
// between candidates — for our case, arg[0]'s tag mismatch with
// the sbitmap overload eliminates it.
struct Tag1 { int a; };
struct Tag2 { int b; };

static inline void overloaded_fn(struct Tag1 *m, int b) { (void)m; (void)b; }
extern int overloaded_fn(struct Tag2 *m, int b);

int overloaded_fn(struct Tag2 *m, int b) { (void)m; (void)b; return 1; }

// Force arg[1]'s resolved_type to be NULL by using a void-cast or
// equivalent gap. The arithmetic expression below is the kind of
// expression sema's typing path tends to leave un-typed in real
// gcc 4.8 code (mode-and-ENUM mix). We use 'i + (int)(*p)' where
// *p deref of a void* won't have a clean resolved_type.
int main() {
    struct Tag2 t2;
    void *p = (void*)"a";
    int i = 0;
    return overloaded_fn(&t2, i + (int)((const char*)p)[0]);
}
