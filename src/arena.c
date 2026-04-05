/*
 * arena.c — Arena (bump) allocator and growable array (Vec).
 *
 * All AST nodes, types, and temporary arrays are allocated from an arena.
 * The arena survives through the entire pipeline (parse → sema → codegen)
 * and is freed in one shot at the end. No individual free() calls.
 *
 * The Vec type provides a growable array backed by the arena. Since the
 * arena never frees individual allocations, growing a Vec "wastes" the old
 * backing array — acceptable for a bootstrap tool that processes one
 * translation unit and exits.
 */

#include "sea-front.h"

/* Default arena page size: 64KB */
#define ARENA_DEFAULT_PAGE_SIZE (64 * 1024)

/* Alignment for all arena allocations */
#define ARENA_ALIGN 16

static size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static ArenaPage *new_page(size_t min_size, size_t default_size) {
    size_t size = min_size > default_size ? min_size : default_size;
    ArenaPage *page = malloc(sizeof(ArenaPage) + size);
    if (!page) {
        fprintf(stderr, "sea-front: arena out of memory\n");
        exit(1);
    }
    page->next = NULL;
    page->size = size;
    page->used = 0;
    return page;
}

Arena arena_new(void) {
    Arena a;
    a.cur = new_page(0, ARENA_DEFAULT_PAGE_SIZE);
    a.page_size = ARENA_DEFAULT_PAGE_SIZE;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    size = align_up(size, ARENA_ALIGN);

    /* Try current page */
    if (a->cur->used + size <= a->cur->size) {
        void *p = a->cur->data + a->cur->used;
        a->cur->used += size;
        memset(p, 0, size);
        return p;
    }

    /* Allocate new page (at least big enough for this request) */
    ArenaPage *page = new_page(size, a->page_size);
    page->next = a->cur;
    a->cur = page;
    void *p = page->data;
    page->used = size;
    memset(p, 0, size);
    return p;
}

void arena_free_all(Arena *a) {
    ArenaPage *page = a->cur;
    while (page) {
        ArenaPage *next = page->next;
        free(page);
        page = next;
    }
    a->cur = NULL;
}

/*
 * Vec — growable array, arena-backed.
 *
 * Growth strategy: semi-exponential. Double capacity up to 4096 elements,
 * then grow by 50% thereafter. This keeps amortized push O(1) while
 * avoiding excessive waste for large arrays.
 *
 * Since the arena never frees, growing "wastes" the old backing array.
 * This is acceptable: the parser's arrays are small (function params,
 * block statements, argument lists — rarely > 100 elements), and the
 * tool processes one TU and exits.
 */

#define VEC_INITIAL_CAP 4

Vec vec_new(Arena *arena) {
    Vec v;
    v.data = NULL;
    v.len = 0;
    v.cap = 0;
    v.arena = arena;
    return v;
}

void vec_push(Vec *v, void *item) {
    if (v->len >= v->cap) {
        int new_cap;
        if (v->cap == 0) {
            new_cap = VEC_INITIAL_CAP;
        } else if (v->cap < 4096) {
            new_cap = v->cap * 2;        /* double up to 4K */
        } else {
            new_cap = v->cap + v->cap / 2; /* +50% after that */
        }

        void **new_data = arena_alloc(v->arena, new_cap * sizeof(void *));
        if (v->data && v->len > 0)
            memcpy(new_data, v->data, v->len * sizeof(void *));
        /* old v->data is arena-wasted — no free needed */
        v->data = new_data;
        v->cap = new_cap;
    }
    v->data[v->len++] = item;
}

void *vec_get(Vec *v, int index) {
    return v->data[index];
}
