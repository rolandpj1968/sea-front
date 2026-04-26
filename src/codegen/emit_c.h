/*
 * emit_c.h — AST → C codegen.
 *
 * Walks a sema-resolved AST (and the template-instantiation pass's
 * cloned subtrees) and emits valid C source to stdout. Covers:
 *
 *   - Built-in types, expressions, statements, control flow.
 *   - Classes / structs / unions: layout emit, ctor + dtor lowering
 *     (goto-chain cleanup), method dispatch, virtual via vtable.
 *   - Operator overloads (binary, unary, subscript, assign, etc.) —
 *     N4659 §16.5 [over.oper].
 *   - Free-function overloads — disambiguated via the canonical
 *     param-type-suffix mangle (see emit_free_func_symbol +
 *     codegen/mangle.h).
 *   - Templates after instantiation — the cloned ND_CLASS_DEF /
 *     ND_FUNC_DEF nodes look like ordinary classes and functions
 *     to this pass.
 *   - GNU __asm("name") symbol-rename declarator-suffix and
 *     extern "C" linkage suppression of mangling.
 *
 * Still missing or partial:
 *   - Exceptions (throw / try / catch — only stubs).
 *   - Coroutines (out of scope).
 *   - C++20 modules / concepts (out of scope).
 *
 * Per-helper N4659 citations live in emit_c.c.
 */

#ifndef EMIT_C_H
#define EMIT_C_H

#include "../parse/parse.h"

/* When true (the default), emit #line directives mapping the C
 * output back to the original C++ source positions. Gcc error
 * messages and gdb will reference the C++ source, not the
 * generated C. Disable with --no-line-directives when debugging
 * the emitted C itself. */
extern bool g_emit_line_directives;

void emit_c(Node *tu);

#endif
