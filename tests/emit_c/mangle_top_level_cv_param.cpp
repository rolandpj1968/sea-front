// EXPECT: 42
// Itanium C++ ABI §5.1.5 / N4659 §13.1.4.4: top-level cv on
// parameter types is DROPPED during mangling. 'f(T*)' and
// 'f(T* const)' resolve to the same overload and link to the
// same mangled symbol. Sea-front previously emitted 'const_'
// in the mangling for any is_const Type — for TY_PTR with the
// const on the pointer itself (top-level), that produced
//   _p_const_T_ptr_pe_ vs _p_T_ptr_pe_
// at the def vs the caller, link failed.
//
// Concrete: gcc 4.8's
//   void mangle_decl (const tree decl);  // const on outer ptr
// was the trigger — 8+ unresolved refs to mangle_decl plus 7+
// for mangle_conv_op_name_for_type plus a long tail in cc1plus.
//
// Reproducer pattern: define a function via a TYPEDEF for a
// pointer, with const at top-level on the param. Have a caller
// see the bare (non-const) signature. Both must link.
struct Box { int v; };
typedef struct Box *BoxPtr;

// Forward decl as seen by callers: bare BoxPtr.
extern int box_get(BoxPtr p);

// Definition: top-level const on the param ('const BoxPtr p' is
// 'Box * const p' — the pointer is const, NOT the pointee).
int box_get(const BoxPtr p) { return p->v; }

int main() {
    struct Box b = {42};
    return box_get(&b);
}
