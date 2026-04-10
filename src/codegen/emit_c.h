/*
 * emit_c.h — AST → C codegen.
 *
 * Walks a sema-resolved AST and emits valid C source. The first slice
 * handles only built-in arithmetic types: functions, statements,
 * expressions, control flow on int/long/float/etc. Classes, templates,
 * member access, and overloads are out of scope.
 *
 * Output is written to stdout.
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
