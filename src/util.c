/*
 * util.c — File I/O, error reporting, memory helpers.
 */

#define _POSIX_C_SOURCE 200809L
#include "sea-front.h"

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "sea-front: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (!p) {
        fprintf(stderr, "sea-front: out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) {
        fprintf(stderr, "sea-front: out of memory\n");
        exit(1);
    }
    return p;
}

File *sf_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "sea-front: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /*
     * Allocate with padding of NUL bytes past the end of the file.
     * The lexer needs this for:
     *   - Up to 4-byte lookahead (e.g. the <:: digraph exception)
     *   - memcmp in raw string delimiter matching (up to 16-byte delimiter
     *     + 1 for the closing quote = 17 bytes of lookahead past ')')
     * 32 bytes of padding covers all cases with margin.
     */
    #define READ_PADDING 32
    char *contents = xcalloc(1, size + READ_PADDING + 1);
    long nread = (long)fread(contents, 1, size, fp);
    fclose(fp);

    if (nread != size) {
        fprintf(stderr, "sea-front: error reading '%s'\n", path);
        free(contents);
        return NULL;
    }
    /* contents[size] through contents[size+4] are already NUL from xcalloc */

    File *file = xmalloc(sizeof(File));
    file->name = xstrdup(path);
    file->contents = contents;
    file->size = (int)size;
    return file;
}

_Noreturn void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sea-front: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/*
 * Print an error with source location.
 * Finds the line containing loc and prints file:line:col with a caret.
 */
_Noreturn void error_at(const char *filename, const char *source,
                        char *loc, const char *fmt, ...) {
    /* Find line number and start of line */
    int line = 1;
    const char *line_start = source;
    for (const char *p = source; p < loc; p++) {
        if (*p == '\n') {
            line++;
            line_start = p + 1;
        }
    }
    int col = (int)(loc - line_start) + 1;

    /* Find end of line */
    const char *line_end = loc;
    while (*line_end && *line_end != '\n')
        line_end++;

    /* Print location */
    fprintf(stderr, "%s:%d:%d: error: ", filename, line, col);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    /* Print the source line */
    fprintf(stderr, "%.*s\n", (int)(line_end - line_start), line_start);

    /* Print the caret */
    for (int i = 0; i < col - 1; i++)
        fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^\n");

    exit(1);
}

_Noreturn void error_tok(Token *tok, const char *fmt, ...) {
    if (!tok || !tok->file || !tok->file->contents || !tok->loc) {
        va_list ap;
        va_start(ap, fmt);
        if (tok && tok->file && tok->file->name)
            fprintf(stderr, "%s:%d: ", tok->file->name, tok->line);
        else if (tok)
            fprintf(stderr, "line %d: ", tok->line);
        fprintf(stderr, "error: ");
        vfprintf(stderr, fmt, ap);
        if (tok && tok->loc)
            fprintf(stderr, " (near '%.*s')", tok->len > 20 ? 20 : tok->len,
                    tok->loc);
        fprintf(stderr, "\n");
        va_end(ap);
        exit(2);
    }
    /* Reformat as error_at */
    int line = 1;
    const char *line_start = tok->file->contents;
    for (const char *p = tok->file->contents; p < tok->loc; p++) {
        if (*p == '\n') {
            line++;
            line_start = p + 1;
        }
    }
    int col = (int)(tok->loc - line_start) + 1;

    const char *line_end = tok->loc;
    while (*line_end && *line_end != '\n')
        line_end++;

    fprintf(stderr, "%s:%d:%d: error: ", tok->file->name, line, col);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    fprintf(stderr, "%.*s\n", (int)(line_end - line_start), line_start);

    for (int i = 0; i < col - 1; i++)
        fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
    fprintf(stderr, "^\n");

    exit(1);
}
