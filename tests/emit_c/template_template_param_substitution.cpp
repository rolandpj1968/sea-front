// EXPECT: 7
// gcc 4.8 hash_table<Descriptor, Allocator = xcallocator> pattern.
// A class template with a template-template parameter that has a
// default. The cloned method body's Alloc<X>::factor() call must
// substitute Alloc → DefaultAlloc (or whatever the user-supplied
// or default class-template name is) so the call mangles to the
// actual instantiation's symbol.
//
// Without this fix, the cloned body emitted
// 'sf__Alloc_t_int_te___factor_*' (the literal TT-param name) but
// the only definition was 'sf__DefaultAlloc_t_int_te___factor_*'
// — link failed. 53 distinct undefined refs of this shape across
// the gcc 4.8 cc1plus link.
//
// Standard: N4659 §17.2/3 [temp.param] (a template-template
// parameter accepts any class-template argument; §17.7.1
// [temp.inst] (instantiation substitutes the argument throughout
// the template body, including in nested template-ids).
//
// Sea-front's encoding: TT-params are recognised by their leading
// 'template' keyword token (param->tok->kind == TK_KW_TEMPLATE)
// since both TT-params and regular type-params have param.ty == NULL.
// SubstMap carries a tt_bound_name field set at instantiation time;
// clone.c rewrites ND_QUALIFIED parts[0] and ND_TEMPLATE_ID name
// to the bound name during cloning.

template<typename T>
struct DefaultAlloc { static int factor() { return 3; } };

template<typename Desc,
         template<typename T> class Alloc = DefaultAlloc>
struct Box {
    int v;
    int triple();
};

template<typename Desc,
         template<typename T> class Alloc>
int Box<Desc, Alloc>::triple() { return Alloc<int>::factor() * v; }

struct Hasher {};

int main() {
    Box<Hasher> b; b.v = 2;
    return b.triple() + 1;   // (3*2) + 1 = 7
}
