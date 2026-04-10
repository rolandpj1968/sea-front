/*
 * main.c — sea-front driver program.
 *
 * Usage: sea-front [options] <file>
 *
 * Options:
 *   --dump-tokens    Print token list and exit (skips parse/sema/codegen)
 *   --dump-ast       Parse and print AST and exit (skips sema/codegen)
 *   --emit-c         Parse, sema, lower to C on stdout (the main use case)
 *   --std=c++17      Set C++ standard (default: c++17)
 *   --std=c++20      Enable C++20 features
 *   --std=c++23      Enable C++23 features
 *
 * With NO action flag (no --dump-* and no --emit-c) the driver
 * reads, lexes, and parses the input — silently — then exits with
 * status 0 if the parse succeeded. This 'parse-only' mode is what
 * the libstdc++ smoke harness uses to find parser gaps.
 */

#include "sea-front.h"
#include "sema/sema.h"
#include "template/instantiate.h"
#include "codegen/emit_c.h"

static void dump_tokens(TokenArray ta) {
    for (int i = 0; i < ta.len && ta.tokens[i].kind != TK_EOF; i++) {
        Token *t = &ta.tokens[i];
        printf("%s:%d:%d: %-20s '%.*s'",
               t->file->name, t->line, t->col,
               token_kind_name(t->kind),
               t->len, t->loc);

        /* Show literal values for numeric/char tokens */
        if (t->kind == TK_NUM)
            printf("  val=%lld", (long long)t->ival);
        else if (t->kind == TK_FNUM)
            printf("  val=%g", t->fval);
        else if (t->kind == TK_CHAR)
            printf("  val=%lld", (long long)t->ival);

        /* Show encoding prefix */
        if (t->enc != ENC_NONE) {
            const char *enc_names[] = {
                [ENC_NONE] = "",
                [ENC_U8] = "u8",
                [ENC_LITTLE_U] = "u",
                [ENC_BIG_U] = "U",
                [ENC_L] = "L",
            };
            printf("  enc=%s", enc_names[t->enc]);
        }

        /* Show UDL suffix */
        if (t->ud_suffix)
            printf("  ud_suffix='%.*s'", t->ud_suffix_len, t->ud_suffix);

        if (t->is_raw)
            printf("  raw");

        printf("\n");
    }
}

static void usage(void) {
    fprintf(stderr, "usage: sea-front [--dump-tokens] [--dump-ast] [--emit-c] [--no-lines] [--std=c++17|20|23] <file>\n");
    exit(1);
}

int main(int argc, char **argv) {
    bool do_dump_tokens = false;
    bool do_dump_ast = false;
    bool do_emit_c = false;
    CppStandard std = CPP17;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            do_dump_tokens = true;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            do_dump_ast = true;
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            do_emit_c = true;
        } else if (strcmp(argv[i], "--no-lines") == 0) {
            g_emit_line_directives = false;
        } else if (strcmp(argv[i], "--std=c++17") == 0) {
            std = CPP17;
        } else if (strcmp(argv[i], "--std=c++20") == 0) {
            std = CPP20;
        } else if (strcmp(argv[i], "--std=c++23") == 0) {
            std = CPP23;
        } else if (argv[i][0] == '-') {
            error("unknown option: %s", argv[i]);
        } else {
            if (filename)
                error("multiple input files not supported");
            filename = argv[i];
        }
    }

    if (!filename)
        usage();

    File *file = sf_read_file(filename);
    if (!file)
        return 1;

    TokenArray tokens = tokenize(file);

    if (do_dump_tokens) {
        dump_tokens(tokens);
        return 0;
    }

    Arena arena = arena_new();
    Node *ast = parse(tokens, &arena, std);

    if (do_dump_ast) {
        dump_ast(ast, 0);
        arena_free_all(&arena);
        return 0;
    }

    if (do_emit_c) {
        sema_run(ast, &arena);
        template_instantiate(ast, &arena);
        sema_run(ast, &arena);  /* re-run on instantiated nodes */
        emit_c(ast);
        arena_free_all(&arena);
        return 0;
    }

    /* Parse-only mode: no action flag was passed. We've already
     * lexed and parsed; just clean up and exit successfully. The
     * libstdc++ smoke harness uses this mode — a parser gap shows
     * up as a non-zero exit. Sema and codegen are reachable via
     * --emit-c above. */
    arena_free_all(&arena);
    return 0;
}
