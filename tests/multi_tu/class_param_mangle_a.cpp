// Half of the class-param-mangle multi-TU test. This TU sees
// only ONE decl of f(): the class-param overload. Without
// d3d6d61, free_func_name_is_overloaded looked at per-TU
// overload count, so this TU would happily emit 'f' unmangled.
// The other TU sees TWO overloads of f and mangles its call —
// link then fails with "undefined reference to f_p_Box_ptr_pe_".
//
// gcc 4.8 reproducer: bitmap.c included only bitmap.h
// (bitmap_set_bit looked like a single non-overloaded function,
// def emitted unmangled), reginfo.c also included sbitmap.h
// (saw both overloads, call was mangled). 3203 undefined refs in
// cc1plus until d3d6d61 made class-param-shape force mangling
// regardless of per-TU overload count. Itanium C++ ABI §5.1 /
// N4659 §10.5 [dcl.link]: every C++ free function with a
// class-typed param is mangled by signature.

struct Box { int v; };

int f(Box *p) {
    return p->v;
}
