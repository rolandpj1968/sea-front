#!/bin/sh
# bootstrap.sh — Build sea-front from a trusted C compiler.
#
# This is the minimal trust-chain build: one C compiler, one shell,
# no make, no autotools, no dependencies beyond POSIX.
#
# Usage: CC=cc ./bootstrap.sh
#
# The Makefile is the developer convenience for incremental builds
# and testing. This script is the bootstrap recipe.

set -e

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2 -Wall}"

echo "Building sea-front with: $CC $CFLAGS"

# sea-front: C++ to C transpiler
$CC $CFLAGS -o sea-front \
    src/main.c \
    src/util.c \
    src/arena.c \
    src/lex/tokenize.c \
    src/lex/unicode.c \
    src/parse/parser.c \
    src/parse/expr.c \
    src/parse/stmt.c \
    src/parse/decl.c \
    src/parse/type.c \
    src/parse/lookup.c \
    src/parse/ast_dump.c \
    src/sema/sema.c \
    src/template/instantiate.c \
    src/template/clone.c \
    src/codegen/emit_c.c \
    src/codegen/mangle.c

echo "Built: sea-front"

# mcpp: vendored preprocessor
$CC -O2 -w -o mcpp-bin \
    vendor/mcpp/main.c \
    vendor/mcpp/directive.c \
    vendor/mcpp/eval.c \
    vendor/mcpp/expand.c \
    vendor/mcpp/support.c \
    vendor/mcpp/system.c \
    vendor/mcpp/mbchar.c

echo "Built: mcpp-bin"
echo "Bootstrap complete."
