/*
 * tests/test_mangle_itanium.c — Itanium ABI mangling fixture driver.
 *
 * Constructs Type values directly (no parser) and feeds them through
 * the Itanium type encoder. Output is one line per fixture in
 * `<label>: <mangled>` form so a diff against the expected file
 * catches regressions, and a separate `make compare-itanium` rule
 * cross-checks against `g++ -c | nm | c++filt` for the same C++
 * signatures.
 */
#include "codegen/mangle.h"
#include "parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build small Types inline. We don't go through the parser so we
 * just zero-init and set the fields each fixture needs. */
static Type t_void   = { .kind = TY_VOID };
static Type t_bool   = { .kind = TY_BOOL };
static Type t_char   = { .kind = TY_CHAR };
static Type t_uchar  = { .kind = TY_CHAR,  .is_unsigned = true };
static Type t_short  = { .kind = TY_SHORT };
static Type t_ushort = { .kind = TY_SHORT, .is_unsigned = true };
static Type t_int    = { .kind = TY_INT };
static Type t_uint   = { .kind = TY_INT,   .is_unsigned = true };
static Type t_long   = { .kind = TY_LONG };
static Type t_ulong  = { .kind = TY_LONG,  .is_unsigned = true };
static Type t_llong  = { .kind = TY_LLONG };
static Type t_ullong = { .kind = TY_LLONG, .is_unsigned = true };
static Type t_float  = { .kind = TY_FLOAT };
static Type t_double = { .kind = TY_DOUBLE };
static Type t_ldouble = { .kind = TY_LDOUBLE };

static Type t_const_int = { .kind = TY_INT, .is_const = true };

/* Pointer / reference Types. We have to give them distinct identities
 * so two `int*` from different fixtures don't accidentally pointer-
 * compare equal — substitution lookup uses structural equality so
 * this is fine. */
static Type t_int_ptr        = { .kind = TY_PTR, .base = &t_int };
static Type t_int_ptr_2      = { .kind = TY_PTR, .base = &t_int };
static Type t_int_ptr_3      = { .kind = TY_PTR, .base = &t_int };
static Type t_const_int_ptr  = { .kind = TY_PTR, .base = &t_const_int };
static Type t_int_ptr_ptr    = { .kind = TY_PTR, .base = &t_int_ptr };
static Type t_int_ref        = { .kind = TY_REF, .base = &t_int };
static Type t_int_rref       = { .kind = TY_RVALREF, .base = &t_int };
static Type t_void_ptr       = { .kind = TY_PTR, .base = &t_void };
static Type t_int_arr5       = { .kind = TY_ARRAY, .base = &t_int, .array_len = 5 };

/* Active-mangler kind is a runtime global. */
extern MangleKind g_mangle_kind;

/* Mock tokens for class/namespace names. The Token API takes a
 * non-const char*; we use mutable buffers since nothing should
 * write through them. */
static char tok_text_T[] = "T";
static char tok_text_A[] = "A";
static char tok_text_std[] = "std";
static char tok_text_ns[] = "ns";
static char tok_text_foo[] = "foo";

static Token tok_T   = { .kind = TK_IDENT, .loc = tok_text_T,   .len = 1 };
static Token tok_A   = { .kind = TK_IDENT, .loc = tok_text_A,   .len = 1 };
static Token tok_std = { .kind = TK_IDENT, .loc = tok_text_std, .len = 3 };
static Token tok_ns  = { .kind = TK_IDENT, .loc = tok_text_ns,  .len = 2 };
static Token tok_foo = { .kind = TK_IDENT, .loc = tok_text_foo, .len = 3 };

/* Mock declarative regions. A class's class_region's enclosing
 * chain is what the namespace walker traverses. */
static DeclarativeRegion reg_global = { .kind = REGION_NAMESPACE };
static DeclarativeRegion reg_std    = { .kind = REGION_NAMESPACE };
static DeclarativeRegion reg_ns     = { .kind = REGION_NAMESPACE };
static DeclarativeRegion reg_class_T_global = { .kind = REGION_CLASS };
static DeclarativeRegion reg_class_T_std    = { .kind = REGION_CLASS };
static DeclarativeRegion reg_class_T_ns     = { .kind = REGION_CLASS };

static Type t_class_T_global = { .kind = TY_STRUCT, .tag = &tok_T };
static Type t_class_T_std    = { .kind = TY_STRUCT, .tag = &tok_T };
static Type t_class_T_ns     = { .kind = TY_STRUCT, .tag = &tok_T };
static Type t_class_A_global = { .kind = TY_STRUCT, .tag = &tok_A };

static void wire_regions(void) {
    /* std and ns enclose global. Each class region encloses its
     * containing namespace. */
    reg_std.enclosing = &reg_global;
    reg_std.name      = &tok_std;
    reg_ns.enclosing  = &reg_global;
    reg_ns.name       = &tok_ns;

    reg_class_T_global.enclosing = &reg_global;
    reg_class_T_global.owner_type = &t_class_T_global;
    reg_class_T_std.enclosing    = &reg_std;
    reg_class_T_std.owner_type   = &t_class_T_std;
    reg_class_T_ns.enclosing     = &reg_ns;
    reg_class_T_ns.owner_type    = &t_class_T_ns;

    t_class_T_global.class_region = &reg_class_T_global;
    t_class_T_std.class_region    = &reg_class_T_std;
    t_class_T_ns.class_region     = &reg_class_T_ns;
    t_class_A_global.class_region = &reg_class_T_global;
    /* (re-uses the global region; its owner_type is wrong but we
     * never query that on this Type for the fixtures below). */
}

static void run_fixture_type(const char *label, Type *ty) {
    printf("%s: ", label);
    itan_emit_type_for_mangle(ty);
    putchar('\n');
}

static void run_fixture_params(const char *label, Type **params, int n) {
    printf("%s: ", label);
    itan_mangle_param_suffix(params, n);
    putchar('\n');
}

static void run_fixture_method(const char *label, Type *cls, Token *m,
                                Type **params, int np, bool is_const) {
    printf("%s: ", label);
    itan_mangle_class_method_cv(cls, m, params, np, is_const);
    putchar('\n');
}

static void run_fixture_ctor(const char *label, Type *cls,
                              Type **params, int np) {
    printf("%s: ", label);
    mangle_class_ctor(cls, params, np);
    putchar('\n');
}

static void run_fixture_dtor(const char *label, Type *cls) {
    printf("%s: ", label);
    mangle_class_dtor(cls);
    putchar('\n');
}

static void run_fixture_dtor_body(const char *label, Type *cls) {
    printf("%s: ", label);
    mangle_class_dtor_body(cls);
    putchar('\n');
}

int main(void) {
    g_mangle_kind = MANGLE_ITANIUM;
    wire_regions();

    /* Fundamentals — Itanium ABI §5.1.5. */
    run_fixture_type("void",            &t_void);
    run_fixture_type("bool",            &t_bool);
    run_fixture_type("char",            &t_char);
    run_fixture_type("unsigned_char",   &t_uchar);
    run_fixture_type("short",           &t_short);
    run_fixture_type("unsigned_short",  &t_ushort);
    run_fixture_type("int",             &t_int);
    run_fixture_type("unsigned_int",    &t_uint);
    run_fixture_type("long",            &t_long);
    run_fixture_type("unsigned_long",   &t_ulong);
    run_fixture_type("long_long",       &t_llong);
    run_fixture_type("unsigned_long_long", &t_ullong);
    run_fixture_type("float",           &t_float);
    run_fixture_type("double",          &t_double);
    run_fixture_type("long_double",     &t_ldouble);

    /* Compounds */
    run_fixture_type("int_ptr",         &t_int_ptr);
    run_fixture_type("const_int_ptr",   &t_const_int_ptr);
    run_fixture_type("int_ptr_ptr",     &t_int_ptr_ptr);
    run_fixture_type("int_ref",         &t_int_ref);
    run_fixture_type("int_rref",        &t_int_rref);
    run_fixture_type("void_ptr",        &t_void_ptr);

    /* Param lists exercise the substitution table. The expected
     * outputs match what `g++ -c` would emit for the corresponding
     * `void f(<params>)` symbol's parameter portion. */
    {
        Type *p[] = { &t_int };
        run_fixture_params("p_int", p, 1);
    }
    {
        Type *p[] = { &t_int_ptr };
        run_fixture_params("p_int_ptr", p, 1);
    }
    {
        /* Two `int*` Types — distinct pointers, structural-eq dedup
         * to S_ on the second per Itanium ABI §5.1.6.5. */
        Type *p[] = { &t_int_ptr, &t_int_ptr_2 };
        run_fixture_params("p_int_ptr_int_ptr", p, 2);
    }
    {
        Type *p[] = { &t_int_ptr, &t_const_int_ptr };
        run_fixture_params("p_int_ptr_const_int_ptr", p, 2);
    }
    {
        Type *p[] = { &t_int_ptr_ptr };
        run_fixture_params("p_int_ptr_ptr", p, 1);
    }
    {
        /* int* (×3 distinct), const int* (×2 distinct) — second
         * onwards back-reference. */
        Type *p[] = { &t_int_ptr, &t_int_ptr_2, &t_int_ptr_3,
                       &t_const_int_ptr };
        run_fixture_params("p_int_ptr_x3_const_int_ptr", p, 4);
    }
    {
        /* Array decays to pointer at param level. */
        Type *p[] = { &t_int_arr5 };
        run_fixture_params("p_int_array5_decays", p, 1);
    }
    {
        /* No params. */
        run_fixture_params("p_void", NULL, 0);
    }

    /* ---------- Stage 2 fixtures: names + class methods ---------- */

    /* Class type as a parameter. */
    {
        Type *p[] = { &t_class_A_global };
        run_fixture_params("p_class_A_global", p, 1);
    }
    {
        /* `T` from std namespace as a param: NSt1TE. */
        Type *p[] = { &t_class_T_std };
        run_fixture_params("p_class_T_std", p, 1);
    }
    {
        /* `T` from ns namespace: N2ns1TE. */
        Type *p[] = { &t_class_T_ns };
        run_fixture_params("p_class_T_ns", p, 1);
    }

    /* Method calls. */
    run_fixture_method("m_global_T_foo_void",
        &t_class_T_global, &tok_foo, NULL, 0, false);
    run_fixture_method("m_global_T_foo_int",
        &t_class_T_global, &tok_foo, (Type*[]){ &t_int }, 1, false);
    run_fixture_method("m_global_T_foo_const",
        &t_class_T_global, &tok_foo, NULL, 0, true);
    run_fixture_method("m_std_T_foo_void",
        &t_class_T_std, &tok_foo, NULL, 0, false);
    run_fixture_method("m_ns_T_foo_void",
        &t_class_T_ns, &tok_foo, NULL, 0, false);
    {
        /* Method `void T::foo(T*, T*)` — second T* should sub. */
        Type t_T_ptr  = { .kind = TY_PTR, .base = &t_class_T_global };
        Type t_T_ptr2 = { .kind = TY_PTR, .base = &t_class_T_global };
        Type *p[] = { &t_T_ptr, &t_T_ptr2 };
        run_fixture_method("m_global_T_foo_T_ptr_T_ptr",
            &t_class_T_global, &tok_foo, p, 2, false);
    }

    /* ---------- Stage 3-style fixtures: deeper sub chains -------- */
    {
        /* `void T::foo(const T*, const T*, T*)` — first const-T then
         * const-T* should be pushed; second const-T* subs; the bare
         * T* doesn't because it differs in cv. */
        Type t_const_T = { .kind = TY_STRUCT, .tag = &tok_T,
                            .class_region = &reg_class_T_global,
                            .is_const = true };
        Type t_cT_ptr  = { .kind = TY_PTR, .base = &t_const_T };
        Type t_cT_ptr2 = { .kind = TY_PTR, .base = &t_const_T };
        Type t_T_ptr  = { .kind = TY_PTR, .base = &t_class_T_global };
        Type *p[] = { &t_cT_ptr, &t_cT_ptr2, &t_T_ptr };
        run_fixture_method("m_global_T_foo_constT_ptr_x2_T_ptr",
            &t_class_T_global, &tok_foo, p, 3, false);
    }
    {
        /* `void T::foo(T*, T**, T*, T**)` — exercises pushing both
         * T* and T**, then back-referencing each. */
        Type t_T_ptr      = { .kind = TY_PTR, .base = &t_class_T_global };
        Type t_T_ptr_2    = { .kind = TY_PTR, .base = &t_class_T_global };
        Type t_T_pp       = { .kind = TY_PTR, .base = &t_T_ptr };
        Type t_T_pp_2     = { .kind = TY_PTR, .base = &t_T_ptr };
        Type *p[] = { &t_T_ptr, &t_T_pp, &t_T_ptr_2, &t_T_pp_2 };
        run_fixture_method("m_global_T_foo_Tp_Tpp_Tp_Tpp",
            &t_class_T_global, &tok_foo, p, 4, false);
    }
    {
        /* Deep-chain: `void T::foo(int*, T*, int*, T*, int*)` —
         * separately tracks `int*` and `T*` subs through interleaving. */
        Type t_T_ptr   = { .kind = TY_PTR, .base = &t_class_T_global };
        Type t_T_ptr_2 = { .kind = TY_PTR, .base = &t_class_T_global };
        Type t_ip   = { .kind = TY_PTR, .base = &t_int };
        Type t_ip_2 = { .kind = TY_PTR, .base = &t_int };
        Type t_ip_3 = { .kind = TY_PTR, .base = &t_int };
        Type *p[] = { &t_ip, &t_T_ptr, &t_ip_2, &t_T_ptr_2, &t_ip_3 };
        run_fixture_method("m_global_T_foo_ip_Tp_ip_Tp_ip",
            &t_class_T_global, &tok_foo, p, 5, false);
    }

    /* ---------- Stage 4 fixtures: template-id encoding ---------- */
    {
        /* `vec<int>` (global). Type carries n_template_args=1,
         * template_args=[int]. Class scope is the global region. */
        static char tok_text_vec[] = "vec";
        static Token tok_vec = { .kind = TK_IDENT, .loc = tok_text_vec, .len = 3 };
        static DeclarativeRegion reg_class_vec = { .kind = REGION_CLASS };
        static Type t_vec_int_targ = { .kind = TY_INT };
        static Type *vec_int_targs[] = { &t_vec_int_targ };
        static Type t_vec_int = { .kind = TY_STRUCT, .tag = &tok_vec,
                                   .template_args = vec_int_targs,
                                   .n_template_args = 1 };
        reg_class_vec.enclosing = &reg_global;
        reg_class_vec.owner_type = &t_vec_int;
        t_vec_int.class_region = &reg_class_vec;
        run_fixture_type("class_vec_int", &t_vec_int);
        Type *p1[] = { &t_vec_int };
        run_fixture_params("p_vec_int", p1, 1);

        /* `vec<int>, vec<int>` — second is sub. */
        static Type t_vec_int2 = { .kind = TY_STRUCT, .tag = &tok_vec,
                                    .template_args = vec_int_targs,
                                    .n_template_args = 1,
                                    .class_region = &reg_class_vec };
        Type *p2[] = { &t_vec_int, &t_vec_int2 };
        run_fixture_params("p_vec_int_x2", p2, 2);

        /* `vec<int>, vec<double>, vec<int>` — distinct args. */
        static Type t_double = { .kind = TY_DOUBLE };
        static Type *vec_dbl_targs[] = { &t_double };
        static Type t_vec_dbl = { .kind = TY_STRUCT, .tag = &tok_vec,
                                   .template_args = vec_dbl_targs,
                                   .n_template_args = 1,
                                   .class_region = &reg_class_vec };
        Type *p3[] = { &t_vec_int, &t_vec_dbl, &t_vec_int2 };
        run_fixture_params("p_vec_int_dbl_int", p3, 3);

        /* `vec<int>*` */
        static Type t_vec_int_ptr = { .kind = TY_PTR, .base = &t_vec_int };
        Type *p4[] = { &t_vec_int_ptr };
        run_fixture_params("p_vec_int_ptr", p4, 1);
    }

    /* NTTP literals — `ic<int, 42>`, `ic<int, -7>`, `b<true>`. */
    {
        static char tok_text_ic[] = "ic";
        static Token tok_ic = { .kind = TK_IDENT, .loc = tok_text_ic, .len = 2 };
        static DeclarativeRegion reg_class_ic = { .kind = REGION_CLASS };
        reg_class_ic.enclosing = &reg_global;

        static char tok_text_42[] = "42";
        static char tok_text_neg7[] = "-7";
        static Token tok_42  = { .kind = TK_NUM, .loc = tok_text_42,   .len = 2 };
        static Token tok_neg7 = { .kind = TK_NUM, .loc = tok_text_neg7, .len = 2 };
        static Type t_nttp_42  = { .kind = TY_NTTP_VALUE, .tag = &tok_42 };
        static Type t_nttp_neg7 = { .kind = TY_NTTP_VALUE, .tag = &tok_neg7 };
        static Type t_int_arg = { .kind = TY_INT };
        static Type *ic_42_args[]  = { &t_int_arg, &t_nttp_42 };
        static Type *ic_neg_args[] = { &t_int_arg, &t_nttp_neg7 };
        static Type t_ic_42  = { .kind = TY_STRUCT, .tag = &tok_ic,
                                  .template_args = ic_42_args,
                                  .n_template_args = 2,
                                  .class_region = &reg_class_ic };
        static Type t_ic_neg = { .kind = TY_STRUCT, .tag = &tok_ic,
                                  .template_args = ic_neg_args,
                                  .n_template_args = 2,
                                  .class_region = &reg_class_ic };
        Type *p_ic42[]  = { &t_ic_42 };
        Type *p_icneg[] = { &t_ic_neg };
        run_fixture_params("p_ic_int_42", p_ic42, 1);
        run_fixture_params("p_ic_int_neg7", p_icneg, 1);

        static char tok_text_b[] = "b";
        static Token tok_b = { .kind = TK_IDENT, .loc = tok_text_b, .len = 1 };
        static DeclarativeRegion reg_class_b = { .kind = REGION_CLASS };
        reg_class_b.enclosing = &reg_global;

        static char tok_text_true[]  = "true";
        static char tok_text_false[] = "false";
        static Token tok_true  = { .kind = TK_KW_TRUE,  .loc = tok_text_true,  .len = 4 };
        static Token tok_false = { .kind = TK_KW_FALSE, .loc = tok_text_false, .len = 5 };
        static Type t_nttp_true  = { .kind = TY_NTTP_VALUE, .tag = &tok_true };
        static Type t_nttp_false = { .kind = TY_NTTP_VALUE, .tag = &tok_false };
        static Type *b_true_args[]  = { &t_nttp_true };
        static Type *b_false_args[] = { &t_nttp_false };
        static Type t_b_true  = { .kind = TY_STRUCT, .tag = &tok_b,
                                   .template_args = b_true_args,
                                   .n_template_args = 1,
                                   .class_region = &reg_class_b };
        static Type t_b_false = { .kind = TY_STRUCT, .tag = &tok_b,
                                   .template_args = b_false_args,
                                   .n_template_args = 1,
                                   .class_region = &reg_class_b };
        Type *p_btrue[]  = { &t_b_true };
        Type *p_bfalse[] = { &t_b_false };
        run_fixture_params("p_b_true",  p_btrue,  1);
        run_fixture_params("p_b_false", p_bfalse, 1);
    }

    /* ---------- Stage 5 fixtures: ctors / dtors ---------- */
    run_fixture_ctor("c_global_T_void", &t_class_T_global, NULL, 0);
    run_fixture_ctor("c_global_T_int",  &t_class_T_global,
                      (Type*[]){ &t_int }, 1);
    run_fixture_ctor("c_std_T_void", &t_class_T_std, NULL, 0);
    run_fixture_dtor("d_global_T",     &t_class_T_global);
    run_fixture_dtor("d_std_T",        &t_class_T_std);
    run_fixture_dtor_body("db_global_T", &t_class_T_global);

    return 0;
}
