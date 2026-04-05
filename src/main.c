/*
 * main.c — sea-front driver program.
 *
 * Usage: sea-front [--dump-tokens] <file>
 */

#include "sea-front.h"

static void dump_tokens(Token *tok) {
    for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
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

int main(int argc, char **argv) {
    bool do_dump_tokens = false;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            do_dump_tokens = true;
        } else if (argv[i][0] == '-') {
            error("unknown option: %s", argv[i]);
        } else {
            if (filename)
                error("multiple input files not supported");
            filename = argv[i];
        }
    }

    if (!filename) {
        fprintf(stderr, "usage: sea-front [--dump-tokens] <file>\n");
        return 1;
    }

    File *file = read_file(filename);
    if (!file)
        return 1;

    Token *tok = tokenize(file);

    if (do_dump_tokens)
        dump_tokens(tok);

    return 0;
}
