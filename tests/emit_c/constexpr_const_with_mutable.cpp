// EXPECT: 0
// `constexpr T x{...}` lowers to `static const T x = {...}` in C
// per N4659 §10.1.5/9 [dcl.constexpr] (constexpr implies const).
// If T carries a mutable transitive member, the const lands the
// storage in .rodata and the mutable-write cast-trick segfaults.
//
// Two emit paths can introduce the C-level const:
//   - the Type's own `is_const` (handled by emit_var_decl_inner)
//   - the storage flag DECL_CONSTEXPR (handled by
//     emit_var_storage_flags_for_type)
// Both must consult class_has_mutable_field_transitive — including
// for template-instantiated classes like Foo<Bar> where the mutable
// lives on Bar reached through Foo's substituted `val` member.
//
// Reduced from g++.dg/cpp1y/constexpr-mutable2.C.

extern "C" void abort(void);

struct Bar { mutable int val_{}; };

template <class T>
struct Foo { T val; };

int main() {
    constexpr Foo<Bar> x{};
    x.val.val_ = 42;
    if (x.val.val_ != 42) abort();
    return 0;
}
