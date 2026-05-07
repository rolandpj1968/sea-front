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

int main(void) {
    g_mangle_kind = MANGLE_ITANIUM;

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

    return 0;
}
