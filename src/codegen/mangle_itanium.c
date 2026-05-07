/*
 * src/codegen/mangle_itanium.c — Itanium C++ ABI symbol mangling.
 *
 * Itanium C++ ABI <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling>.
 *
 * Selected at codegen entry by --mangling=itanium. Each public entry
 * point starts a fresh per-symbol substitution table, walks the type
 * / class tree per the ABI's traversal rules, and emits the mangled
 * name to stdout.
 *
 * Stage 0 (this file's initial commit): scaffolding only — every
 * entry point aborts with a clear marker so the dispatch wiring can
 * be exercised by tests without producing wrong symbols. Stages 1+
 * fill in the encoding incrementally; see
 * memory/project_itanium_mangling_slice.md.
 */
#include "mangle.h"
#include "../parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void itan_unimpl(const char *what) {
    fprintf(stderr,
        "sea-front: --mangling=itanium not yet implemented (%s)\n"
        "  Stage 0 scaffolding only; see "
        "memory/project_itanium_mangling_slice.md for the plan.\n",
        what);
    abort();
}

void itan_mangle_class_tag(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_tag");
}

void itan_mangle_class_method_cv(Type *class_type, Token *method_name,
                                  Type **param_types, int nparams,
                                  bool is_const) {
    (void)class_type; (void)method_name; (void)param_types;
    (void)nparams; (void)is_const;
    itan_unimpl("mangle_class_method_cv");
}

void itan_mangle_class_ctor(Type *class_type,
                             Type **param_types, int nparams) {
    (void)class_type; (void)param_types; (void)nparams;
    itan_unimpl("mangle_class_ctor");
}

void itan_mangle_class_dtor(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_dtor");
}

void itan_mangle_class_dtor_body(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_dtor_body");
}

void itan_mangle_class_vtable_type(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_vtable_type");
}

void itan_mangle_class_vtable_instance(Type *class_type) {
    (void)class_type; itan_unimpl("mangle_class_vtable_instance");
}

void itan_emit_type_for_mangle(Type *ty) {
    (void)ty; itan_unimpl("emit_type_for_mangle");
}

void itan_mangle_param_suffix(Type **param_types, int nparams) {
    (void)param_types; (void)nparams; itan_unimpl("mangle_param_suffix");
}

int itan_mangle_param_suffix_to_buf(Type **param_types, int nparams,
                                     char *buf, int pos, int max) {
    (void)param_types; (void)nparams; (void)buf; (void)pos; (void)max;
    itan_unimpl("mangle_param_suffix_to_buf");
    return pos;
}

int itan_mangle_type_to_buf(Type *ty, char *buf, int pos, int max) {
    (void)ty; (void)buf; (void)pos; (void)max;
    itan_unimpl("mangle_type_to_buf");
    return pos;
}
