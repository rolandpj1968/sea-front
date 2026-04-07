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

void emit_c(Node *tu);

#endif
