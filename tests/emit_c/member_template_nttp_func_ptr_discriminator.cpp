// EXPECT: 0
// Member-template with a function-pointer NTTP, three call sites with
// three distinct callback pointers. Each instantiation must mangle
// distinctly or three identical __SF_INLINE bodies collide at C-level.
//
// Real-world shape: gcc 4.8 hash_table::traverse — header declares
//   template <typename Arg, int (*Callback)(value_type **, Arg)>
//   void traverse(Arg);
// and tree-ssa-threadupdate.c calls
//   redirection_data.traverse<ssa_local_info_t*, ssa_create_duplicates>(...)
//   redirection_data.traverse<ssa_local_info_t*, ssa_fixup_template_block>(...)
//   redirection_data.traverse<ssa_local_info_t*, ssa_redirect_edges>(...)
//
// Pre-fix: Itanium NTTP mangle fell back to `Li0E` for non-integral
// NTTPs (function-pointer types lack a builtin_code), so all three
// rendered `_ZN10holder...E8callIPiXadL?EEEEv` → identical symbol,
// __SF_INLINE bodies became C-level redefinitions.
// Fix: emit_type_for_mangle on TY_NTTP_VALUE now appends
// `XadL_Z<n><tag>EE` when no builtin_code applies, embedding the
// entity name to differentiate.

extern "C" void abort();

int seen = 0;

int cb_one(int *p, int a)   { (void)p; seen += 1 * a; return 0; }
int cb_two(int *p, int a)   { (void)p; seen += 2 * a; return 0; }
int cb_three(int *p, int a) { (void)p; seen += 4 * a; return 0; }

template<typename Desc>
struct holder {
    template<typename Arg, int (*Callback)(typename Desc::value_type **, Arg)>
    void call(Arg a) {
        typename Desc::value_type *vp = 0;
        typename Desc::value_type **slot = &vp;
        Callback(slot, a);
    }
};

struct intd { typedef int value_type; };

int main() {
    holder<intd> h;
    h.call<int, cb_one>(10);
    h.call<int, cb_two>(10);
    h.call<int, cb_three>(10);
    if (seen != 10 + 20 + 40) abort();
    return 0;
}
