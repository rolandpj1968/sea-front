/*
 * emit_c.c — AST → C codegen.
 *
 * Walks a parsed (and sema'd) translation unit and emits C source
 * to stdout. The lowering targets a fixed C subset that any C99
 * compiler can build (notably tcc-from-mescc — sea-front's whole
 * point is to be the C/C++ bridge in the bootstrappable.org chain;
 * see memory project_bootstrap_north_star).
 *
 * Currently emits:
 *   - Free functions, parameters with all built-in and pointer types
 *   - References (TY_REF / TY_RVALREF) lowered to plain C pointers
 *   - Variable declarations with init or direct-init (T x(args))
 *   - All flow control: return, if/else, while, for, do-while,
 *     break/continue, switch/case/default, goto/label
 *   - Compound statements with cleanup-aware destructor unwinding
 *     (the Slice A-D dtor lowering — see below)
 *   - Full binary/unary/ternary/assignment expressions
 *   - All literals (int, float, char, bool, string, nullptr)
 *   - Identifiers (with this-> rewriting for class members and
 *     __sf_base.<...> walking for inherited members)
 *   - Function calls including method calls (direct dispatch via
 *     mangled name) and virtual calls (indirect dispatch via vptr)
 *   - Class definitions: struct layout, ctors, dtors, ctor/dtor
 *     synthesis, member-initializer lists, vptr install + vtable
 *     emission, base-subobject embedding for non-virtual inheritance
 *
 * Mangling: every emitted symbol routes through src/codegen/mangle.c
 * and the Mangler vtable (default scheme: human-readable 'sf__' form;
 * see docs/mangling.md). Direct fputs of unmangled forms is
 * deliberately avoided so a future Itanium adapter is a one-vtable
 * change.
 *
 * NOT YET EMITTED (gaps that need their own slice):
 *   - Operator overloading (parsed, but not lowered to op_* calls)
 *   - Templates beyond opaque parse (no instantiation)
 *   - Exceptions (try/throw/catch — the cleanup chain is the
 *     substrate but the THROW unwind variant isn't wired up)
 *   - Virtual inheritance, virtual destructors in vtable slots
 *   - RTTI / dynamic_cast / typeid
 *   - Coroutines / modules / concepts (C++20+)
 *
 * The output is intentionally minimal — no formatting cleverness,
 * no register allocation, no optimisation. The trust story is
 * "read the lowered C and verify it matches the source", so
 * readability beats brevity.
 */

#include <stdio.h>
#include <string.h>
#include "emit_c.h"
#include "mangle.h"
#include "../sea-front.h"

bool g_emit_line_directives = true;

/* Two-phase emit: structs first, then method bodies. */
static int g_emit_phase = 0;
enum { PHASE_STRUCTS = 1, PHASE_METHODS = 2 };

static int g_indent = 0;
/* Currently-emitting class definition (for ctor member-init walking
 * and inherited-member access rewriting). Set by emit_class_def
 * before each method body, restored after. NULL outside class
 * member emission. */
static Node *g_current_class_def = NULL;

static void emit_indent(void) {
    for (int i = 0; i < g_indent; i++) fputs("    ", stdout);
}

/* ------------------------------------------------------------------ */
/* Per-function codegen state for destructor lowering (Slice B)       */
/*                                                                    */
/* When the function body contains any local with a non-trivial dtor  */
/* we lower 'return expr' as:                                         */
/*     __retval = expr; __unwind = 1; goto __cleanup_<innermost>;     */
/* Each block carrying cleanups emits a label, runs its dtors, and    */
/* conditionally chains outward via 'if (__unwind) goto <parent>'.    */
/* The function epilogue runs 'return __retval;'.                     */
/*                                                                    */
/* This is per-function state, not nested blocks, so a flat module    */
/* global is fine — codegen is single-threaded and not reentrant.     */
/* ------------------------------------------------------------------ */

/* Per-var cleanup tracking — Slice C extension.
 *
 * The 'live' stack now holds two kinds of entries:
 *   CL_VAR  — a constructed dtor-bearing local. Has a label_id and
 *             a var_decl; its label is both a dtor call site and a
 *             chain target for any unwind type.
 *   CL_LOOP — a loop boundary marker. Pushed by emit_while/do/for
 *             when a loop has cleanups in its body. Carries the
 *             loop's break_label and cont_label. Markers do not run
 *             dtors; they exist so break/continue chains know where
 *             to terminate.
 *
 * Walking the stack outward:
 *   RETURN: skip CL_LOOP markers, find the next CL_VAR; if none,
 *           target __SF_epilogue.
 *   BREAK:  use the FIRST entry encountered (CL_VAR or CL_LOOP).
 *           A var chains to the next thing; a marker is the loop
 *           boundary and resolves to its break_label.
 *   CONT:   same as BREAK, but a marker resolves to its cont_label.
 *
 * For chain-out emission at the bottom of a block, the three target
 * walks usually collapse to the same answer (no enclosing loop, or
 * no marker between us and the next var). We compare and emit a
 * single 'if (__SF_unwind) goto X' when they coincide, otherwise a
 * three-way conditional. */

typedef enum CleanupKind {
    CL_VAR,
    CL_LOOP,
} CleanupKind;

typedef struct CleanupEntry {
    CleanupKind kind;
    int         label_id;       /* CL_VAR: cleanup label / CL_LOOP: break_label */
    int         cont_label_id;  /* CL_LOOP only */
    Node       *var_decl;       /* CL_VAR only */
} CleanupEntry;

#define CLEANUP_LIVE_MAX 64

static struct {
    bool          func_has_cleanups;  /* function-wide flag, set by pre-scan */
    int           next_label_id;      /* fresh-id counter */
    CleanupEntry  live[CLEANUP_LIVE_MAX];
    int           nlive;              /* number of live entries currently */
} g_cf;

/* For RETURN: walk outward, skip CL_LOOP markers, find first CL_VAR.
 * Returns label_id, or -1 meaning "jump straight to __SF_epilogue". */
static int find_return_target_from(int top) {
    for (int i = top - 1; i >= 0; i--)
        if (g_cf.live[i].kind == CL_VAR)
            return g_cf.live[i].label_id;
    return -1;
}

/* For BREAK: the topmost live entry IS the next target — both
 * CL_VAR (chain to its label) and CL_LOOP (its break_label) live
 * in the same field. Returns -1 only if break is used outside any
 * loop or var scope (malformed; should already be a sema error). */
static int find_break_target_from(int top) {
    if (top <= 0) return -1;
    return g_cf.live[top - 1].label_id;
}

/* For CONT: same as break, but CL_LOOP resolves to its cont_label
 * instead of its break_label. CL_VAR is unchanged. */
static int find_cont_target_from(int top) {
    if (top <= 0) return -1;
    CleanupEntry *e = &g_cf.live[top - 1];
    return (e->kind == CL_VAR) ? e->label_id : e->cont_label_id;
}

/* Top-of-stack views for ND_RETURN/BREAK/CONTINUE rewrite sites. */
static int return_target(void) { return find_return_target_from(g_cf.nlive); }
static int break_target(void)  { return find_break_target_from(g_cf.nlive); }
static int cont_target(void)   { return find_cont_target_from(g_cf.nlive); }

/* Forward decls for the hoist helpers below — both call into the
 * regular emitters, which appear later in the file. */
static void emit_expr(Node *n);
static void emit_type(Type *ty);
static void emit_mangled_class_tag(Type *class_type);

/*
 * Emit a "C++: ..." comment showing the original C++ declaration.
 * Extracts the source line from the Token's loc pointer (which points
 * into the source buffer). Prints from the start of the line up to
 * the first '{', ';', or end of line — just the declaration part.
 */
static void emit_source_comment(Token *tok) {
    if (!tok || !tok->loc) return;
    /* Walk backward to the start of the line */
    const char *line_start = tok->loc;
    while (line_start > tok->loc - 200 && line_start[-1] != '\n')
        line_start--;
    /* Skip leading whitespace */
    const char *p = line_start;
    while (*p == ' ' || *p == '\t') p++;
    int len = 0;
    while (p[len] && p[len] != '\n' && p[len] != '{' && len < 200)
        len++;
    /* Trim trailing whitespace */
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t'))
        len--;
    if (len <= 0) return;
    fputs("/* C++: ", stdout);
    /* Write the source line, escaping any embedded comment-close
     * sequences that would break the C comment. */
    for (int i = 0; i < len; i++) {
        if (p[i] == '*' && i + 1 < len && p[i + 1] == '/') {
            fputs("*\\/", stdout);
            i++;  /* skip the '/' */
        } else {
            fputc(p[i], stdout);
        }
    }
    fputs(" */\n", stdout);
    if (g_emit_line_directives && tok->line > 0) {
        /* Standard C #line directive: tells the C compiler that the
         * NEXT line came from the given source position. The file
         * name comes from the Token's file field if available,
         * otherwise we omit it (line-only form). */
        if (tok->file && tok->file->name)
            fprintf(stdout, "#line %d \"%s\"\n", tok->line,
                    tok->file->name);
        else
            fprintf(stdout, "#line %d\n", tok->line);
    }
}

/* Slice D-Hoist temp materialization.
 *
 * Walks an expression tree post-order, looking for ND_CALL nodes
 * whose return type is a class with a non-trivial dtor. For each
 * such call, emits a synthesized local declaration of the form
 *     struct T __SF_temp_<n> = <call>;
 * before the enclosing statement, tags the call node with its
 * temp name, and pushes the temp onto g_cf.live[] so its dtor
 * fires through the cleanup chain at end of block.
 *
 * The post-order walk means inner temps are hoisted first; by
 * the time we emit the outer temp's initializer the inner
 * sub-expressions already have codegen_temp_name set, so emit_expr
 * substitutes the local name verbatim.
 *
 * Lifetime divergence from C++: temps are scoped to the enclosing
 * BLOCK rather than to the full-expression. Their dtors fire in
 * the right relative order (most-recently-constructed first), but
 * later than the C++ standard requires. Observable only when a
 * temp's dtor has a side effect that the next user statement
 * reads back. Documented; mini-block isolation per full-expression
 * is a possible future refinement. */

static bool is_class_temp_call(Node *n) {
    if (!n || n->kind != ND_CALL || !n->resolved_type ||
        n->resolved_type->kind != TY_STRUCT || n->codegen_temp_name)
        return false;
    /* Standard case: class-typed call with non-trivial dtor needs
     * hoisting so the dtor fires at the right scope. */
    if (n->resolved_type->has_dtor) return true;
    /* Ctor-call shape: 'Foo(args)' where the callee is a type-name
     * needs hoisting even when Foo has no dtor — there's no Foo()
     * function in the lowered C, only the mangled
     * sf__Foo__ctor(struct sf__Foo *, ...). The temp gets a slot
     * but no cleanup-chain registration. */
    if (n->call.callee && n->call.callee->kind == ND_IDENT) {
        Declaration *d = n->call.callee->ident.resolved_decl;
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG))
            return true;
    }
    return false;
}

static void hoist_emit_decl(Node *call) {
    int id = g_cf.next_label_id++;
    /* The temp name lives in a small static buffer pool — one per
     * temp, since multiple temps can be live in the same statement
     * and codegen_temp_name is just a borrowed pointer. */
    static char name_pool[CLEANUP_LIVE_MAX][24];
    static int name_idx = 0;
    if (name_idx >= CLEANUP_LIVE_MAX) name_idx = 0;  /* wrap */
    char *name = name_pool[name_idx++];
    snprintf(name, 24, "__SF_temp_%d", id);

    /* Detect ctor-call shape: callee is an ND_IDENT whose
     * resolved_decl is a type-name (ENTITY_TYPE). For these we
     * emit the two-line construction form
     *     struct sf__T __SF_temp_<n>;
     *     sf__T__ctor(&__SF_temp_<n>, args);
     * instead of the single-line assignment-init form
     *     struct sf__T __SF_temp_<n> = some_call();
     * because Foo(args) isn't a function call in C — there's no
     * symbol named 'Foo' (only the mangled sf__Foo__ctor). */
    bool is_ctor_call = false;
    if (call->call.callee && call->call.callee->kind == ND_IDENT) {
        Declaration *d = call->call.callee->ident.resolved_decl;
        if (d && (d->entity == ENTITY_TYPE || d->entity == ENTITY_TAG))
            is_ctor_call = true;
    }

    if (is_ctor_call) {
        emit_indent();
        emit_type(call->resolved_type);
        fprintf(stdout, " %s;\n", name);
        emit_indent();
        mangle_class_ctor(call->resolved_type);
        fprintf(stdout, "(&%s", name);
        for (int i = 0; i < call->call.nargs; i++) {
            fputs(", ", stdout);
            emit_expr(call->call.args[i]);
        }
        fputs(");\n", stdout);
    } else {
        /* Function call returning a class — direct copy form. */
        emit_indent();
        emit_type(call->resolved_type);
        fprintf(stdout, " %s = ", name);
        emit_expr(call);  /* call's children may already be substituted */
        fputs(";\n", stdout);
    }

    /* Tag the call so emit_expr now substitutes the temp name. */
    call->codegen_temp_name = name;

    /* Push as a CL_VAR onto the live stack so the cleanup chain
     * runs the dtor at end of block. Skip the push for trivially-
     * destructible types (no dtor to call) — the temp is still a
     * named local that the rest of the expression uses, but it
     * doesn't need cleanup tracking. */
    if (call->resolved_type && call->resolved_type->has_dtor &&
        g_cf.nlive < CLEANUP_LIVE_MAX) {
        g_cf.live[g_cf.nlive].kind = CL_VAR;
        g_cf.live[g_cf.nlive].label_id = id;
        g_cf.live[g_cf.nlive].cont_label_id = -1;
        g_cf.live[g_cf.nlive].var_decl = call;  /* not an ND_VAR_DECL — flagged via codegen_temp_name */
        g_cf.nlive++;
    }
}

static void hoist_temps_in_expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_CALL:
        if (n->call.callee) hoist_temps_in_expr(n->call.callee);
        for (int i = 0; i < n->call.nargs; i++)
            hoist_temps_in_expr(n->call.args[i]);
        if (is_class_temp_call(n)) hoist_emit_decl(n);
        return;
    case ND_BINARY:
    case ND_ASSIGN:
        /* Both share the 'binary' member layout. */
        hoist_temps_in_expr(n->binary.lhs);
        hoist_temps_in_expr(n->binary.rhs);
        return;
    case ND_UNARY:
    case ND_POSTFIX:
        /* Both share the 'unary' member layout. */
        hoist_temps_in_expr(n->unary.operand);
        return;
    case ND_TERNARY:
        hoist_temps_in_expr(n->ternary.cond);
        hoist_temps_in_expr(n->ternary.then_);
        hoist_temps_in_expr(n->ternary.else_);
        return;
    case ND_MEMBER:
        hoist_temps_in_expr(n->member.obj);
        return;
    case ND_SUBSCRIPT:
        hoist_temps_in_expr(n->subscript.base);
        hoist_temps_in_expr(n->subscript.index);
        return;
    case ND_CAST:
        hoist_temps_in_expr(n->cast.operand);
        return;
    case ND_COMMA:
        hoist_temps_in_expr(n->comma.lhs);
        hoist_temps_in_expr(n->comma.rhs);
        return;
    default:
        return;
    }
}

/* Hoist any class temps from the expressions belonging to a single
 * statement. Called by emit_block before emit_indent for each
 * statement, so synthesized declarations land at the same indent
 * level as the user statement.
 *
 * Special case: 'T x = make();' is direct-init from a class call
 * — the call's return value IS x via struct copy in C, no separate
 * temp needed. We skip hoisting the top-level call but still
 * recurse into its arguments (which may carry their own temps). */
static void hoist_stmt_temps(Node *s) {
    if (!s) return;
    /* Note: we deliberately do NOT gate on func_has_cleanups here.
     * Even functions without any non-trivial dtors may contain
     * ctor-call expressions ('Foo(7)') that need to be materialized
     * to a synthesized local — the lowered C has no symbol named
     * 'Foo', only the mangled sf__Foo__ctor. The hoist fires; the
     * cleanup-chain registration in hoist_emit_decl is what's
     * gated on has_dtor, so a trivially-destructible temp gets a
     * local but no CL_VAR entry. */
    switch (s->kind) {
    case ND_VAR_DECL: {
        Node *init = s->var_decl.init;
        if (!init) return;
        bool direct_init =
            init->kind == ND_CALL && init->resolved_type &&
            init->resolved_type->kind == TY_STRUCT &&
            s->var_decl.ty && s->var_decl.ty->kind == TY_STRUCT;
        if (direct_init) {
            /* Recurse into the call's children only — don't hoist
             * the call itself. */
            if (init->call.callee) hoist_temps_in_expr(init->call.callee);
            for (int i = 0; i < init->call.nargs; i++)
                hoist_temps_in_expr(init->call.args[i]);
        } else {
            hoist_temps_in_expr(init);
        }
        return;
    }
    case ND_EXPR_STMT:
        hoist_temps_in_expr(s->expr_stmt.expr);
        return;
    case ND_RETURN:
        hoist_temps_in_expr(s->ret.expr);
        return;
    /* ND_IF/WHILE/DO/FOR conditions are NOT hoisted at the outer
     * block level — that would put the temp in extended-lifetime
     * scope, observing the wrong dtor timing. Instead, the
     * per-statement handler in emit_stmt does a structural rewrite:
     * synth int outside, mini-block around the cond evaluation,
     * temp dtors fire before the branch runs. See ND_IF case in
     * emit_stmt. */
    default:
        return;
    }
}

/* Are we currently inside any enclosing loop? Used to decide
 * whether ND_BREAK/CONTINUE need the cleanup-aware rewrite or
 * can be lowered as a plain C break/continue. */
static bool break_needs_cleanup(void) {
    /* If the topmost live entry is a CL_LOOP marker, no cleanup
     * vars are in flight inside the current loop and a plain C
     * 'break' will exit the loop correctly. Otherwise we have at
     * least one var to destroy first. */
    if (!g_cf.func_has_cleanups) return false;
    if (g_cf.nlive == 0) return false;
    return g_cf.live[g_cf.nlive - 1].kind == CL_VAR;
}

/* Pre-scan: does any block in this subtree declare a class instance
 * with a non-trivial dtor, OR contain an expression that creates a
 * class temporary (call returning class with has_dtor)? Either case
 * needs the cleanup machinery (locals __retval/__unwind, __epilogue
 * label, return rewrite, per-block cleanup chain). */
static bool subtree_has_cleanups(Node *n) {
    if (!n) return false;
    /* First: a call returning a non-trivially destructible class
     * is a temporary site that Slice D-Hoist will materialize. */
    if (n->kind == ND_CALL && n->resolved_type &&
        n->resolved_type->kind == TY_STRUCT && n->resolved_type->has_dtor)
        return true;
    switch (n->kind) {
    case ND_VAR_DECL:
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
            n->var_decl.ty->has_dtor) return true;
        return subtree_has_cleanups(n->var_decl.init);
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            if (subtree_has_cleanups(n->block.stmts[i])) return true;
        return false;
    case ND_IF:
        return subtree_has_cleanups(n->if_.cond) ||
               subtree_has_cleanups(n->if_.then_) ||
               subtree_has_cleanups(n->if_.else_);
    case ND_WHILE:
        return subtree_has_cleanups(n->while_.cond) ||
               subtree_has_cleanups(n->while_.body);
    case ND_DO:
        return subtree_has_cleanups(n->do_.cond) ||
               subtree_has_cleanups(n->do_.body);
    case ND_FOR:
        return subtree_has_cleanups(n->for_.init) ||
               subtree_has_cleanups(n->for_.cond) ||
               subtree_has_cleanups(n->for_.inc) ||
               subtree_has_cleanups(n->for_.body);
    case ND_RETURN:
        return subtree_has_cleanups(n->ret.expr);
    case ND_EXPR_STMT:
        return subtree_has_cleanups(n->expr_stmt.expr);
    /* Expression nodes — recurse into children. */
    case ND_BINARY:
    case ND_ASSIGN:
        return subtree_has_cleanups(n->binary.lhs) ||
               subtree_has_cleanups(n->binary.rhs);
    case ND_UNARY:
    case ND_POSTFIX:
        return subtree_has_cleanups(n->unary.operand);
    case ND_TERNARY:
        return subtree_has_cleanups(n->ternary.cond) ||
               subtree_has_cleanups(n->ternary.then_) ||
               subtree_has_cleanups(n->ternary.else_);
    case ND_CALL:
        if (subtree_has_cleanups(n->call.callee)) return true;
        for (int i = 0; i < n->call.nargs; i++)
            if (subtree_has_cleanups(n->call.args[i])) return true;
        return false;
    case ND_MEMBER:
        return subtree_has_cleanups(n->member.obj);
    case ND_SUBSCRIPT:
        return subtree_has_cleanups(n->subscript.base) ||
               subtree_has_cleanups(n->subscript.index);
    case ND_CAST:
        return subtree_has_cleanups(n->cast.operand);
    case ND_COMMA:
        return subtree_has_cleanups(n->comma.lhs) ||
               subtree_has_cleanups(n->comma.rhs);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Name mangling                                                      */
/* ------------------------------------------------------------------ */

/* Emit the mangled struct/class tag for a class type via the
 * Mangler framework (see docs/mangling.md and mangle.c). With the
 * default human scheme this produces 'sf__<NS>__<Class>' where
 * the namespace prefix walks the class's enclosing chain. The
 * '?' fallback is for class types whose tag wasn't recorded
 * (anonymous structs, currently). */
static int g_anon_counter = 0;

/* Dedup for free function declarations/definitions.
 * C doesn't support overloading, so we keep the first declaration
 * of each function name and skip subsequent ones. Handles C++ system
 * headers that provide const/non-const overloads (strchr, abs, div, etc.)
 * and inline functions included from multiple header paths. */
static struct { const char *loc; int len; } g_func_seen[8192];
static int g_func_nseen = 0;

static bool func_dedup_check(Token *name) {
    if (!name) return false;
    for (int i = 0; i < g_func_nseen; i++) {
        if (g_func_seen[i].len == name->len &&
            memcmp(g_func_seen[i].loc, name->loc, name->len) == 0)
            return true;  /* already seen */
    }
    if (g_func_nseen < 8192) {
        g_func_seen[g_func_nseen].loc = name->loc;
        g_func_seen[g_func_nseen].len = name->len;
        g_func_nseen++;
    }
    return false;  /* first time */
}

static void emit_mangled_class_tag(Type *class_type) {
    if (!class_type || !class_type->tag) {
        /* Anonymous struct/union — generate a unique name.
         * C11 allows anonymous struct/union members inside structs,
         * but we need a name for type references and forward decls. */
        fprintf(stdout, "__sf_anon_%d", g_anon_counter++);
        return;
    }
    mangle_class_tag(class_type);
}

/* Number of direct (non-virtual) base classes — N4659 §13 [class.derived].
 * Walks the class's REGION_CLASS base list. The bases are recorded
 * during class-body parsing (parse_type_specifiers calls
 * region_add_base). For inheritance LAYOUT, codegen embeds each
 * base as a struct field and chains ctors/dtors through them. */
static int class_nbases(Type *class_type) {
    if (!class_type || !class_type->class_region) return 0;
    return class_type->class_region->nbases;
}

/* Get the i-th direct base's TYPE (not just its region). The base's
 * region carries owner_type back to the Type — that's what codegen
 * needs to mangle the base's tag, walk the base's class_def, etc. */
static Type *class_base(Type *class_type, int i) {
    if (!class_type || !class_type->class_region) return NULL;
    if (i < 0 || i >= class_type->class_region->nbases) return NULL;
    return class_type->class_region->bases[i]->owner_type;
}

/* Find a path from 'current' to a base 'target' through the
 * non-virtual inheritance graph. Depth-first; first match wins,
 * matching the lookup convention in lookup.c. Path indices are
 * written to path[] and the length (number of inheritance hops)
 * is returned. Zero means 'not found' (or current == target — no
 * walk needed). Bounded by max_depth to avoid runaway in malformed
 * graphs. */
static int find_base_path(Type *current, Type *target,
                          int *path, int max_depth) {
    if (!current || !target || current == target || max_depth <= 0)
        return 0;
    int nb = class_nbases(current);
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(current, b);
        if (!base) continue;
        if (base == target) {
            path[0] = b;
            return 1;
        }
        int sub = find_base_path(base, target, path + 1, max_depth - 1);
        if (sub > 0) {
            path[0] = b;
            return sub + 1;
        }
    }
    return 0;
}

/* Emit '__sf_base.' (or '__sf_base<N>.') for each step in the path.
 * Used when chaining through inherited members:
 *     this->__sf_base.__sf_base.x
 * The trailing '.' is included so the caller can append the final
 * member name directly. Pass len == 0 for a no-op. */
static void emit_base_chain(int *path, int len) {
    for (int i = 0; i < len; i++) {
        int b = path[i];
        if (b == 0) fputs("__sf_base.", stdout);
        else        fprintf(stdout, "__sf_base%d.", b);
    }
}

/* Walk a class's member list to find a method by name and report
 * whether it's virtual. Used by call-site emission to decide
 * between direct dispatch (sf__Class__m(&obj, args)) and indirect
 * dispatch (obj.__sf_vptr->m(&obj, args)). */
static bool method_is_virtual(Type *class_type, Token *method_name) {
    if (!class_type || !class_type->class_def || !method_name) return false;
    Node *cdef = class_type->class_def;
    for (int i = 0; i < cdef->class_def.nmembers; i++) {
        Node *m = cdef->class_def.members[i];
        if (!m) continue;
        if (m->kind == ND_FUNC_DEF && m->func.name &&
            m->func.name->len == method_name->len &&
            memcmp(m->func.name->loc, method_name->loc, method_name->len) == 0)
            return m->func.is_virtual;
        if (m->kind == ND_VAR_DECL && m->var_decl.name &&
            m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC &&
            m->var_decl.name->len == method_name->len &&
            memcmp(m->var_decl.name->loc, method_name->loc, method_name->len) == 0)
            return m->var_decl.is_virtual;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Type emission                                                      */
/* ------------------------------------------------------------------ */

static void emit_type(Type *ty) {
    if (!ty) { fputs("/*?*/ int", stdout); return; }

    if (ty->is_const)    fputs("const ", stdout);
    if (ty->is_volatile) fputs("volatile ", stdout);

    switch (ty->kind) {
    case TY_VOID:    fputs("void", stdout); return;
    case TY_BOOL:    fputs("_Bool", stdout); return;  /* C spelling */
    case TY_CHAR:    fputs(ty->is_unsigned ? "unsigned char" : "char", stdout); return;
    case TY_CHAR16:  fputs("char16_t", stdout); return;
    case TY_CHAR32:  fputs("char32_t", stdout); return;
    case TY_WCHAR:   fputs("wchar_t", stdout); return;
    case TY_SHORT:   fputs(ty->is_unsigned ? "unsigned short" : "short", stdout); return;
    case TY_INT:     fputs(ty->is_unsigned ? "unsigned int" : "int", stdout); return;
    case TY_LONG:    fputs(ty->is_unsigned ? "unsigned long" : "long", stdout); return;
    case TY_LLONG:   fputs(ty->is_unsigned ? "unsigned long long" : "long long", stdout); return;
    case TY_FLOAT:   fputs("float", stdout); return;
    case TY_DOUBLE:  fputs("double", stdout); return;
    case TY_LDOUBLE: fputs("long double", stdout); return;
    case TY_PTR:     emit_type(ty->base); fputs("*", stdout); return;
    /* References — emit as pointer in C (caller passes &x). */
    case TY_REF:     emit_type(ty->base); fputs("*", stdout); return;
    case TY_RVALREF: emit_type(ty->base); fputs("*", stdout); return;
    case TY_ARRAY:   emit_type(ty->base); fputs("*", stdout); return;
    case TY_STRUCT:
        fputs("struct ", stdout);
        emit_mangled_class_tag(ty);
        return;
    case TY_UNION:
        fputs("union ", stdout);
        emit_mangled_class_tag(ty);
        return;
    case TY_ENUM:
        fputs("enum ", stdout);
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        return;
    case TY_DEPENDENT:
        /* Template type parameter — should have been substituted by the
         * instantiation pass before codegen. If we reach here, the type
         * is in an uninstantiated template body (skipped by codegen) or
         * a dependent context we haven't resolved. Emit as a comment
         * for debugging. */
        fputs("/*dep:", stdout);
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        fputs("*/ int", stdout);
        return;
    default:
        fputs("/*?*/ int", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Expression emission                                                */
/* ------------------------------------------------------------------ */

static void emit_expr(Node *n);

static const char *binop_str(TokenKind k) {
    switch (k) {
    case TK_PLUS:    return "+";
    case TK_MINUS:   return "-";
    case TK_STAR:    return "*";
    case TK_SLASH:   return "/";
    case TK_PERCENT: return "%";
    case TK_LT:      return "<";
    case TK_LE:      return "<=";
    case TK_GT:      return ">";
    case TK_GE:      return ">=";
    case TK_EQ:      return "==";
    case TK_NE:      return "!=";
    case TK_LAND:    return "&&";
    case TK_LOR:     return "||";
    case TK_AMP:     return "&";
    case TK_PIPE:    return "|";
    case TK_CARET:   return "^";
    case TK_SHL:     return "<<";
    case TK_SHR:     return ">>";
    case TK_ASSIGN:  return "=";
    case TK_PLUS_ASSIGN:    return "+=";
    case TK_MINUS_ASSIGN:   return "-=";
    case TK_STAR_ASSIGN:    return "*=";
    case TK_SLASH_ASSIGN:   return "/=";
    case TK_PERCENT_ASSIGN: return "%=";
    case TK_AMP_ASSIGN:     return "&=";
    case TK_PIPE_ASSIGN:    return "|=";
    case TK_CARET_ASSIGN:   return "^=";
    case TK_SHL_ASSIGN:     return "<<=";
    case TK_SHR_ASSIGN:     return ">>=";
    default: return "?";
    }
}

/* Map binary operator TokenKind to mangled method suffix.
 * Returns NULL if the operator is not overloadable or we don't
 * support rewriting it yet. Must match the suffixes in
 * emit_operator_method_name. */
static const char *binop_to_operator_suffix(TokenKind op) {
    switch (op) {
    case TK_PLUS:           return "__plus";
    case TK_MINUS:          return "__minus";
    case TK_STAR:           return "__deref";  /* operator* (binary = multiply) */
    case TK_SLASH:          return "__div";
    case TK_PERCENT:        return "__mod";
    case TK_AMP:            return "__bitand";
    case TK_PIPE:           return "__bitor";
    case TK_CARET:          return "__xor";
    case TK_SHL:            return "__lshift";
    case TK_SHR:            return "__rshift";
    case TK_EQ:             return "__eq";
    case TK_NE:             return "__ne";
    case TK_LT:             return "__lt";
    case TK_GT:             return "__gt";
    case TK_LE:             return "__le";
    case TK_GE:             return "__ge";
    case TK_LAND:           return "__land";
    case TK_LOR:            return "__lor";
    case TK_PLUS_ASSIGN:    return "__plus_assign";
    case TK_MINUS_ASSIGN:   return "__minus_assign";
    case TK_STAR_ASSIGN:    return "__mul_assign";
    case TK_SLASH_ASSIGN:   return "__div_assign";
    default:                return NULL;
    }
}

static const char *unop_str(TokenKind k) {
    switch (k) {
    case TK_PLUS:  return "+";
    case TK_MINUS: return "-";
    case TK_STAR:  return "*";
    case TK_AMP:   return "&";
    case TK_EXCL:  return "!";
    case TK_TILDE: return "~";
    case TK_INC:   return "++";
    case TK_DEC:   return "--";
    default: return "?";
    }
}

static void emit_token_text(Token *t) {
    if (!t) { fputs("?", stdout); return; }
    fprintf(stdout, "%.*s", t->len, t->loc);
}

static void emit_expr(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_NUM:
        if (n->num.is_signed)
            fprintf(stdout, "%lld", (long long)n->num.lo);
        else
            fprintf(stdout, "%lluU", (unsigned long long)n->num.lo);
        return;
    case ND_FNUM:
        fprintf(stdout, "%g", n->fnum.fval);
        return;
    case ND_CHAR:
        if (n->chr.tok) fprintf(stdout, "%.*s", n->chr.tok->len, n->chr.tok->loc);
        return;
    case ND_BOOL_LIT:
        /* C doesn't have true/false without stdbool.h. Emit 1/0. */
        if (n->tok && n->tok->len == 4 &&
            memcmp(n->tok->loc, "true", 4) == 0)
            fputc('1', stdout);
        else
            fputc('0', stdout);
        return;
    case ND_NULLPTR:
        fputs("((void*)0)", stdout);
        return;
    case ND_STR:
        if (n->str.tok) fprintf(stdout, "%.*s", n->str.tok->len, n->str.tok->loc);
        return;
    case ND_IDENT:
        if (n->ident.implicit_this) {
            fputs("this->", stdout);
            /* If the resolved declaration lives in a BASE class of
             * the current class, walk through the __sf_base chain
             * — N4659 §11 [class.derived]. */
            Declaration *d = n->ident.resolved_decl;
            if (d && d->home && d->home->owner_type && g_current_class_def) {
                Type *cur = g_current_class_def->class_def.ty;
                Type *tgt = d->home->owner_type;
                int path[8];
                int len = find_base_path(cur, tgt, path, 8);
                emit_base_chain(path, len);
            }
        }
        emit_token_text(n->ident.name);
        return;
    case ND_BINARY: {
        /* Check if the LHS is a struct type with an overloaded operator.
         * If so, rewrite 'a + b' → 'Class__plus(&a, b)'. */
        Type *lhs_ty = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
        /* Dereference pointer-to-struct (e.g. from reference lowering) */
        if (lhs_ty && (lhs_ty->kind == TY_PTR || lhs_ty->kind == TY_REF))
            lhs_ty = lhs_ty->base;
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag) {
            const char *suffix = binop_to_operator_suffix(n->binary.op);
            if (suffix) {
                mangle_class_tag(lhs_ty);
                fputs(suffix, stdout);
                fputs("(&", stdout);
                emit_expr(n->binary.lhs);
                fputs(", ", stdout);
                emit_expr(n->binary.rhs);
                fputc(')', stdout);
                return;
            }
        }
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        emit_expr(n->binary.rhs);
        fputc(')', stdout);
        return;
    }
    case ND_ASSIGN: {
        /* Compound assignment on struct: a += b → Class__plus_assign(&a, b) */
        Type *lhs_ty = n->binary.lhs ? n->binary.lhs->resolved_type : NULL;
        if (lhs_ty && (lhs_ty->kind == TY_PTR || lhs_ty->kind == TY_REF))
            lhs_ty = lhs_ty->base;
        if (lhs_ty && (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) &&
            lhs_ty->tag && n->binary.op != TK_ASSIGN) {
            const char *suffix = binop_to_operator_suffix(n->binary.op);
            if (suffix) {
                mangle_class_tag(lhs_ty);
                fputs(suffix, stdout);
                fputs("(&", stdout);
                emit_expr(n->binary.lhs);
                fputs(", ", stdout);
                emit_expr(n->binary.rhs);
                fputc(')', stdout);
                return;
            }
        }
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        emit_expr(n->binary.rhs);
        fputc(')', stdout);
        return;
    }
    case ND_UNARY:
        fputc('(', stdout);
        fputs(unop_str(n->unary.op), stdout);
        emit_expr(n->unary.operand);
        fputc(')', stdout);
        return;
    case ND_POSTFIX:
        fputc('(', stdout);
        emit_expr(n->unary.operand);
        fputs(unop_str(n->unary.op), stdout);
        fputc(')', stdout);
        return;
    case ND_TERNARY:
        fputc('(', stdout);
        emit_expr(n->ternary.cond);
        fputs(" ? ", stdout);
        emit_expr(n->ternary.then_);
        fputs(" : ", stdout);
        emit_expr(n->ternary.else_);
        fputc(')', stdout);
        return;
    case ND_CALL: {
        /* Slice D-Hoist: if this call has been hoisted to a synthesized
         * temp local, just emit the local's name and skip the rest of
         * call emission. The temp's initializer (which IS this call)
         * was already emitted ahead of the current statement. */
        if (n->codegen_temp_name) {
            fputs(n->codegen_temp_name, stdout);
            return;
        }
        /* Method call lowering: 'obj.method(args)' / 'p->method(args)'
         * becomes 'Class_method(&obj, args)' / 'Class_method(p, args)'.
         *
         * Detected when the callee is ND_MEMBER and the obj's resolved
         * type is a struct (or pointer-to-struct). The struct's tag
         * gives us the class name to mangle with.
         *
         * Also handles unqualified method calls inside another method
         * body — 'doubled()' inside 'quadrupled()'. The callee is then
         * an ND_IDENT marked implicit_this, and we recover the class
         * via the resolved declaration's home region. */
        Node *callee = n->call.callee;
        if (callee && callee->kind == ND_IDENT &&
            callee->ident.implicit_this &&
            callee->ident.resolved_decl &&
            callee->ident.resolved_decl->type &&
            callee->ident.resolved_decl->type->kind == TY_FUNC &&
            callee->ident.resolved_decl->home &&
            callee->ident.resolved_decl->home->owner_type &&
            callee->ident.resolved_decl->home->owner_type->tag) {
            Type *class_type = callee->ident.resolved_decl->home->owner_type;
            Token *mname = callee->ident.name;
            /* If the method belongs to a base class of the current
             * class, we need to pass &this->__sf_base.<...> instead
             * of bare 'this' so the receiver is the right
             * subobject. find_base_path returns 0 when the method's
             * home IS the current class — no walk needed. */
            int base_path[8];
            int base_len = 0;
            if (g_current_class_def) {
                Type *cur = g_current_class_def->class_def.ty;
                base_len = find_base_path(cur, class_type, base_path, 8);
            }
            if (method_is_virtual(class_type, mname)) {
                /* Virtual dispatch: this->__sf_vptr->m(this, args) */
                fprintf(stdout, "this->__sf_vptr->%.*s(this",
                        mname->len, mname->loc);
            } else {
                mangle_class_method(class_type, mname);
                if (base_len > 0) {
                    /* Inherited method — receiver is the base subobject. */
                    fputs("(&this->", stdout);
                    /* emit_base_chain leaves a trailing '.', so we
                     * emit all but the last hop with the chain helper
                     * and then '__sf_base' (no trailing dot) for the
                     * final hop. */
                    if (base_len > 1)
                        emit_base_chain(base_path, base_len - 1);
                    int last = base_path[base_len - 1];
                    if (last == 0) fputs("__sf_base", stdout);
                    else           fprintf(stdout, "__sf_base%d", last);
                } else {
                    fputs("(this", stdout);
                }
            }
            for (int i = 0; i < n->call.nargs; i++) {
                fputs(", ", stdout);
                emit_expr(n->call.args[i]);
            }
            fputc(')', stdout);
            return;
        }
        if (callee && callee->kind == ND_MEMBER) {
            Node *obj = callee->member.obj;
            Type *ot = obj ? obj->resolved_type : NULL;
            bool obj_is_ptr = ot && ot->kind == TY_PTR;
            if (obj_is_ptr) ot = ot->base;
            if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                ot->tag && callee->member.member) {
                bool virt = method_is_virtual(ot, callee->member.member);
                if (virt) {
                    /* Virtual dispatch: load __sf_vptr then call slot.
                     * Emit the receiver expression once into the slot
                     * and once as the first arg (no temporary —
                     * sufficient for the lvalue/pointer cases we
                     * support). */
                    if (obj_is_ptr) {
                        fputc('(', stdout);
                        emit_expr(obj);
                        fputs(")->__sf_vptr->", stdout);
                    } else {
                        fputc('(', stdout);
                        emit_expr(obj);
                        fputs(").__sf_vptr->", stdout);
                    }
                    fprintf(stdout, "%.*s(",
                            callee->member.member->len,
                            callee->member.member->loc);
                } else {
                    /* Check if the method is inherited from a base.
                     * If so, mangle with the base class and pass
                     * &obj.__sf_base as the this pointer. */
                    Type *method_class = ot;
                    
                    if (ot->class_region) {
                        /* Not in own class? Check bases. */
                        Token *mn = callee->member.member;
                        Declaration *own_d = region_lookup_own(
                            ot->class_region, mn->loc, mn->len);
                        if (!own_d) {
                            for (int bi = 0; bi < ot->class_region->nbases; bi++) {
                                DeclarativeRegion *br = ot->class_region->bases[bi];
                                Declaration *bd = lookup_in_scope(br, mn->loc, mn->len);
                                if (bd && br->owner_type) {
                                    method_class = br->owner_type;
                                    
                                    break;
                                }
                            }
                        }
                    }
                    mangle_class_method(method_class, callee->member.member);
                    fputc('(', stdout);
                }
                /* this argument: address-of for value, as-is for pointer.
                 * For inherited methods, pass &obj.__sf_base instead. */
                int base_idx_for_this = -1;
                if (ot->class_region && callee->member.member) {
                    Token *mn = callee->member.member;
                    Declaration *own_d = region_lookup_own(
                        ot->class_region, mn->loc, mn->len);
                    if (!own_d) {
                        for (int bi = 0; bi < ot->class_region->nbases; bi++) {
                            Declaration *bd = lookup_in_scope(
                                ot->class_region->bases[bi],
                                mn->loc, mn->len);
                            if (bd) { base_idx_for_this = bi; break; }
                        }
                    }
                }
                if (base_idx_for_this >= 0) {
                    fputs("&(", stdout);
                    emit_expr(obj);
                    fputs(").", stdout);
                    if (base_idx_for_this == 0) fputs("__sf_base", stdout);
                    else fprintf(stdout, "__sf_base%d", base_idx_for_this);
                } else if (obj_is_ptr) {
                    emit_expr(obj);
                } else {
                    fputc('&', stdout);
                    emit_expr(obj);
                }
                for (int i = 0; i < n->call.nargs; i++) {
                    fputs(", ", stdout);
                    emit_expr(n->call.args[i]);
                }
                fputc(')', stdout);
                return;
            }
        }
        emit_expr(n->call.callee);
        fputc('(', stdout);
        for (int i = 0; i < n->call.nargs; i++) {
            if (i > 0) fputs(", ", stdout);
            emit_expr(n->call.args[i]);
        }
        fputc(')', stdout);
        return;
    }
    case ND_CAST:
        fputc('(', stdout);
        emit_type(n->cast.ty);
        fputc(')', stdout);
        emit_expr(n->cast.operand);
        return;
    case ND_SIZEOF:
        fputs("sizeof(", stdout);
        if (n->sizeof_.is_type && n->sizeof_.ty)
            emit_type(n->sizeof_.ty);
        else if (n->sizeof_.expr)
            emit_expr(n->sizeof_.expr);
        fputc(')', stdout);
        return;
    case ND_ALIGNOF:
        fputs("_Alignof(", stdout);
        if (n->alignof_.ty) emit_type(n->alignof_.ty);
        fputc(')', stdout);
        return;
    case ND_MEMBER: {
        /* Check if the member lives in a base class and needs
         * __sf_base chain rewriting. */
        Type *obj_ty = n->member.obj ? n->member.obj->resolved_type : NULL;
        if (obj_ty && obj_ty->kind == TY_PTR) obj_ty = obj_ty->base;
        Token *mem = n->member.member;
        bool did_base_rewrite = false;
        if (obj_ty && (obj_ty->kind == TY_STRUCT || obj_ty->kind == TY_UNION) &&
            obj_ty->class_region && mem) {
            /* Check: is this member NOT in the class itself but in a base? */
            Declaration *own = region_lookup_own(obj_ty->class_region,
                                                  mem->loc, mem->len);
            if (!own) {
                /* Not found in own class — check bases */
                /* We need to find WHICH base has this member.
                 * Walk each base's region looking for the member. */
                for (int bi = 0; bi < obj_ty->class_region->nbases; bi++) {
                    DeclarativeRegion *base_r = obj_ty->class_region->bases[bi];
                    Declaration *bd = lookup_in_scope(base_r, mem->loc, mem->len);
                    if (bd) {
                        emit_expr(n->member.obj);
                        fputs(n->member.op == TK_ARROW ? "->" : ".", stdout);
                        if (bi == 0) fputs("__sf_base.", stdout);
                        else fprintf(stdout, "__sf_base%d.", bi);
                        fprintf(stdout, "%.*s", mem->len, mem->loc);
                        did_base_rewrite = true;
                        break;
                    }
                }
            }
        }
        if (!did_base_rewrite) {
            emit_expr(n->member.obj);
            fputs(n->member.op == TK_ARROW ? "->" : ".", stdout);
            if (mem)
                fprintf(stdout, "%.*s", mem->len, mem->loc);
        }
        return;
    }
    case ND_SUBSCRIPT: {
        /* If the base is a struct/union type, this is an overloaded
         * operator[] — emit as a method call. Otherwise, plain C
         * array subscript. */
        Type *base_ty = n->subscript.base ? n->subscript.base->resolved_type : NULL;
        if (base_ty && base_ty->kind == TY_PTR) base_ty = base_ty->base;
        if (base_ty && (base_ty->kind == TY_STRUCT || base_ty->kind == TY_UNION) &&
            base_ty->tag) {
            /* operator[] → mangled method call.
             * If the operator returns a reference (TY_REF / TY_RVALREF
             * → pointer in C), dereference the result. If it returns
             * by value, emit the call directly. */
            bool ref_return = false;
            if (base_ty->class_region) {
                Declaration *d = lookup_in_scope(base_ty->class_region,
                    "operator", 8);
                if (d && d->type && d->type->kind == TY_FUNC &&
                    d->type->ret &&
                    (d->type->ret->kind == TY_REF ||
                     d->type->ret->kind == TY_RVALREF))
                    ref_return = true;
            }
            if (ref_return) fputs("(*", stdout);
            mangle_class_tag(base_ty);
            fputs("__subscript(&", stdout);
            emit_expr(n->subscript.base);
            fputs(", ", stdout);
            emit_expr(n->subscript.index);
            fputc(')', stdout);
            if (ref_return) fputc(')', stdout);
        } else {
            emit_expr(n->subscript.base);
            fputc('[', stdout);
            emit_expr(n->subscript.index);
            fputc(']', stdout);
        }
        return;
    }
    default:
        fputs("/* expr */", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Statement emission                                                 */
/* ------------------------------------------------------------------ */

static void emit_stmt(Node *n);

static void emit_var_decl_inner(Node *n) {
    Type *ty = n->var_decl.ty;
    /* Array declarations: C requires 'int arr[10]' not 'int* arr'.
     * Emit the element type, then the name, then [N]. For unsized
     * arrays (int arr[]) emit just []. For function parameters,
     * emit_type already decays to pointer — this path handles
     * local/global variable declarations only. */
    if (ty && ty->kind == TY_ARRAY) {
        emit_type(ty->base);
        fputc(' ', stdout);
        if (n->var_decl.name)
            fprintf(stdout, "%.*s", n->var_decl.name->len,
                    n->var_decl.name->loc);
        if (ty->array_len >= 0)
            fprintf(stdout, "[%d]", ty->array_len);
        else
            fputs("[]", stdout);
        /* Array init (if any) */
        if (n->var_decl.init) {
            fputs(" = ", stdout);
            emit_expr(n->var_decl.init);
        }
        return;
    }
    emit_type(ty);
    fputc(' ', stdout);
    if (n->var_decl.name)
        fprintf(stdout, "%.*s", n->var_decl.name->len, n->var_decl.name->loc);
    /* Bit-field width — N4659 §12.2.4 [class.bit] */
    if (n->var_decl.bitfield_width) {
        fputs(" : ", stdout);
        emit_expr(n->var_decl.bitfield_width);
    }
    if (n->var_decl.init) {
        fputs(" = ", stdout);
        emit_expr(n->var_decl.init);
    } else if (n->var_decl.has_ctor_init && n->var_decl.ty &&
               n->var_decl.ty->kind != TY_STRUCT &&
               n->var_decl.ctor_nargs >= 1) {
        /* Brace-init / paren-init for a non-class type:
         *   int x{7}    →  int x = 7
         *   int x(7)    →  int x = 7
         * The ctor-call branch in the ND_VAR_DECL emit_stmt case
         * is gated on TY_STRUCT, so non-class direct-inits with
         * args land here. We use the first arg as a copy-init
         * source. Multi-arg forms aren't meaningful for scalars. */
        fputs(" = ", stdout);
        emit_expr(n->var_decl.ctor_args[0]);
    }
}

/* Emit the cleanup chain for entries that were pushed onto live[]
 * after 'saved_nlive'. Emits one (label, dtor) pair per CL_VAR in
 * reverse-declaration order followed by the three-way chain-out.
 *
 * Used by both emit_block (end-of-block cleanup) and the mini-block
 * wrapper (per-full-expression temp cleanup). The shape is identical;
 * the difference is just where it's invoked. */
static void emit_cleanup_chain_for_added(int saved_nlive) {
    int added = 0;
    for (int i = saved_nlive; i < g_cf.nlive; i++)
        if (g_cf.live[i].kind == CL_VAR) added++;
    if (added == 0) return;

    for (int i = g_cf.nlive - 1; i >= saved_nlive; i--) {
        CleanupEntry *e = &g_cf.live[i];
        if (e->kind != CL_VAR) continue;
        Node *v = e->var_decl;
        emit_indent();
        fprintf(stdout, "__SF_cleanup_%d: ;\n", e->label_id);
        emit_indent();
        if (v->codegen_temp_name) {
            /* Slice D-Hoist temp: 'var_decl' is actually the
             * original ND_CALL whose result was hoisted to the
             * synthesized local. Use the call's resolved_type
             * for the class tag and the codegen_temp_name for
             * the address. */
            mangle_class_dtor(v->resolved_type);
            fprintf(stdout, "(&%s);\n", v->codegen_temp_name);
        } else {
            mangle_class_dtor(v->var_decl.ty);
            fprintf(stdout, "(&%.*s);\n",
                    v->var_decl.name->len, v->var_decl.name->loc);
        }
    }
    /* Three-way chain-out. See the comment in emit_block where this
     * was originally written. */
    char rt_buf[40], bt_buf[40], ct_buf[40];
    bool have_break = false, have_cont = false;
    bool top_is_loop = (saved_nlive > 0 &&
                        g_cf.live[saved_nlive - 1].kind == CL_LOOP);

    int rt = find_return_target_from(saved_nlive);
    if (rt >= 0)
        snprintf(rt_buf, sizeof(rt_buf), "__SF_cleanup_%d", rt);
    else
        snprintf(rt_buf, sizeof(rt_buf), "__SF_epilogue");

    int bt = find_break_target_from(saved_nlive);
    if (bt >= 0) {
        have_break = true;
        if (top_is_loop)
            snprintf(bt_buf, sizeof(bt_buf), "__SF_loop_break_%d", bt);
        else
            snprintf(bt_buf, sizeof(bt_buf), "__SF_cleanup_%d", bt);
    }

    int ct = find_cont_target_from(saved_nlive);
    if (ct >= 0) {
        have_cont = true;
        if (top_is_loop)
            snprintf(ct_buf, sizeof(ct_buf), "__SF_loop_cont_%d", ct);
        else
            snprintf(ct_buf, sizeof(ct_buf), "__SF_cleanup_%d", ct);
    }

    bool collapse = have_break && have_cont &&
                    strcmp(rt_buf, bt_buf) == 0 &&
                    strcmp(rt_buf, ct_buf) == 0;
    if (collapse || (!have_break && !have_cont)) {
        emit_indent();
        fprintf(stdout, "__SF_CHAIN_ANY(%s);\n", rt_buf);
    } else {
        emit_indent();
        fprintf(stdout, "__SF_CHAIN_RETURN(%s);\n", rt_buf);
        if (have_break) {
            emit_indent();
            fprintf(stdout, "__SF_CHAIN_BREAK(%s);\n", bt_buf);
        }
        if (have_cont) {
            emit_indent();
            fprintf(stdout, "__SF_CHAIN_CONT(%s);\n", ct_buf);
        }
    }
}

/* Predicate: does an expression contain any class-typed temp
 * (an ND_CALL whose return type is a non-trivially-destructible
 * class)? Used by stmt_has_class_temp to decide whether a
 * statement should be wrapped in a per-full-expression mini-block. */
static bool expr_has_class_temp(Node *e) {
    if (!e) return false;
    if (is_class_temp_call(e)) return true;
    switch (e->kind) {
    case ND_CALL:
        if (expr_has_class_temp(e->call.callee)) return true;
        for (int i = 0; i < e->call.nargs; i++)
            if (expr_has_class_temp(e->call.args[i])) return true;
        return false;
    case ND_BINARY:
    case ND_ASSIGN:
        return expr_has_class_temp(e->binary.lhs) ||
               expr_has_class_temp(e->binary.rhs);
    case ND_UNARY:
    case ND_POSTFIX:
        return expr_has_class_temp(e->unary.operand);
    case ND_TERNARY:
        return expr_has_class_temp(e->ternary.cond) ||
               expr_has_class_temp(e->ternary.then_) ||
               expr_has_class_temp(e->ternary.else_);
    case ND_MEMBER:
        return expr_has_class_temp(e->member.obj);
    case ND_SUBSCRIPT:
        return expr_has_class_temp(e->subscript.base) ||
               expr_has_class_temp(e->subscript.index);
    case ND_CAST:
        return expr_has_class_temp(e->cast.operand);
    case ND_COMMA:
        return expr_has_class_temp(e->comma.lhs) ||
               expr_has_class_temp(e->comma.rhs);
    default:
        return false;
    }
}

/* Predicate: would hoist_stmt_temps actually hoist anything for
 * this statement? Used to decide whether to wrap the statement in
 * a mini-block for per-full-expression temp scoping. */
static bool stmt_has_class_temp(Node *s) {
    if (!s) return false;
    switch (s->kind) {
    case ND_VAR_DECL: {
        Node *init = s->var_decl.init;
        if (!init) return false;
        bool direct_init =
            init->kind == ND_CALL && init->resolved_type &&
            init->resolved_type->kind == TY_STRUCT &&
            s->var_decl.ty && s->var_decl.ty->kind == TY_STRUCT;
        if (direct_init) {
            if (expr_has_class_temp(init->call.callee)) return true;
            for (int i = 0; i < init->call.nargs; i++)
                if (expr_has_class_temp(init->call.args[i])) return true;
            return false;
        }
        return expr_has_class_temp(init);
    }
    case ND_EXPR_STMT:
        return expr_has_class_temp(s->expr_stmt.expr);
    case ND_RETURN:
        return expr_has_class_temp(s->ret.expr);
    default:
        return false;
    }
}

/* Push a CL_VAR for a user-declared class local onto the live stack.
 * Pulled out so emit_block and the mini-block path share it. */
static void push_user_var_cleanup(Node *s) {
    if (!g_cf.func_has_cleanups || !s ||
        s->kind != ND_VAR_DECL || !s->var_decl.ty ||
        s->var_decl.ty->kind != TY_STRUCT || !s->var_decl.ty->has_dtor ||
        !s->var_decl.name || g_cf.nlive >= CLEANUP_LIVE_MAX)
        return;
    g_cf.live[g_cf.nlive].kind = CL_VAR;
    g_cf.live[g_cf.nlive].label_id = g_cf.next_label_id++;
    g_cf.live[g_cf.nlive].cont_label_id = -1;
    g_cf.live[g_cf.nlive].var_decl = s;
    g_cf.nlive++;
}

/* Slice D-MiniBlock: emit one statement with per-full-expression
 * temp scoping. The statement is wrapped in a synthesized C block
 * so any hoisted temps from its expressions die at the end of the
 * full-expression rather than at the end of the surrounding user
 * block.
 *
 * For ND_VAR_DECL the user-declared variable must outlive the
 * mini-block (subsequent statements need to see it), so we split:
 *   <type> name;       (uninitialized, OUTSIDE mini-block)
 *   { hoisted temps; name = init; cleanup chain; }
 *
 * For ND_EXPR_STMT and ND_RETURN no splitting is needed; just
 * wrap the whole statement.
 *
 * For other statement kinds (ND_IF/WHILE/FOR conditions etc.)
 * we currently fall back to the non-mini-block path; per-cond
 * mini-blocking is a future refinement. */
static void emit_stmt_with_miniblock(Node *s) {
    /* For var-decl: declare the user var OUTSIDE the mini-block,
     * register its cleanup BEFORE opening the mini-block (so the
     * temps inside chain to it correctly), then assign inside. */
    bool is_var_decl = (s->kind == ND_VAR_DECL && s->var_decl.name &&
                        s->var_decl.ty);
    if (is_var_decl) {
        emit_indent();
        emit_type(s->var_decl.ty);
        fprintf(stdout, " %.*s;\n",
                s->var_decl.name->len, s->var_decl.name->loc);
        push_user_var_cleanup(s);
    }

    emit_indent();
    fputs("{\n", stdout);
    g_indent++;
    int saved_nlive = g_cf.nlive;

    /* Hoist temps inside the mini-block. They get pushed onto live[]
     * above saved_nlive, so the cleanup chain at the bottom of this
     * mini-block fires their dtors before control leaves. */
    hoist_stmt_temps(s);

    emit_indent();
    if (is_var_decl) {
        /* Emit assignment instead of declaration: 'name = init;'.
         * The init expression has been processed by hoist_temps so
         * any inner calls have been substituted with __SF_temp_<n>. */
        if (s->var_decl.init) {
            fprintf(stdout, "%.*s = ",
                    s->var_decl.name->len, s->var_decl.name->loc);
            emit_expr(s->var_decl.init);
            fputs(";\n", stdout);
        } else {
            fputs(";\n", stdout);
        }
    } else {
        emit_stmt(s);
    }

    emit_cleanup_chain_for_added(saved_nlive);
    g_cf.nlive = saved_nlive;
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

static void emit_block(Node *n) {
    /* Flat blocks (comma-separated declarators, namespace/extern "C" bodies)
     * emit their statements directly without wrapping { } braces, so
     * variables remain visible in the enclosing scope. */
    if (n->block.is_flat) {
        for (int i = 0; i < n->block.nstmts; i++) {
            emit_indent();
            emit_stmt(n->block.stmts[i]);
        }
        return;
    }

    fputs("{\n", stdout);
    g_indent++;

    /* Per-var cleanup label refactor. We walk statements in order
     * and PUSH onto g_cf.live exactly when we see a dtor-bearing
     * var-decl — so at every point during emission innermost-
     * cleanup-label reflects construction state, not block topology. */
    int saved_nlive = g_cf.nlive;

    for (int i = 0; i < n->block.nstmts; i++) {
        Node *s = n->block.stmts[i];
        if (g_cf.func_has_cleanups && stmt_has_class_temp(s)) {
            /* Slice D-MiniBlock: wrap statement so its temps die
             * at end of full-expression, not end of outer block. */
            emit_stmt_with_miniblock(s);
        } else {
            /* Regular path: hoist (no-op if no temps), emit, then
             * register the user var as a CL_VAR if it's a class
             * local with a dtor. */
            hoist_stmt_temps(s);
            emit_indent();
            emit_stmt(s);
            push_user_var_cleanup(s);
        }
    }

    emit_cleanup_chain_for_added(saved_nlive);
    g_cf.nlive = saved_nlive;

    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

static void emit_stmt(Node *n) {
    if (!n) { fputs(";\n", stdout); return; }
    switch (n->kind) {
    case ND_BLOCK:
        emit_block(n);
        return;
    case ND_RETURN:
        if (g_cf.func_has_cleanups) {
            /* Slice B: 'return expr' lowers to one of the __SF_RETURN
             * macros from the prelude. Picking the macro form keeps
             * the emitted C readable, drives the protocol from one
             * place, and means an unbraced 'if (cond) return;' stays
             * safe (the macro wraps a do-while). */

            /* D-Return (NRVO-style move): if the operand is a bare
             * identifier naming the topmost CL_VAR — i.e. a class
             * local that lives at the top of the live stack right
             * now — treat the return as a move. The local IS the
             * return value; its lifetime now belongs to the caller's
             * temp, so we must NOT fire its dtor in the callee.
             *
             * Implementation: bypass the local's own cleanup label
             * and target the next outer label (or __SF_epilogue).
             * This skips the dtor on the return path while leaving
             * fall-through paths to the same label still firing it
             * — so a function with conditional 'return t;' alongside
             * other paths that drop t cleanly destroys t in those
             * other paths. No runtime flag needed. */
            bool moved = false;
            if (n->ret.expr && n->ret.expr->kind == ND_IDENT &&
                g_cf.nlive > 0 &&
                g_cf.live[g_cf.nlive - 1].kind == CL_VAR) {
                Node *top_var = g_cf.live[g_cf.nlive - 1].var_decl;
                Token *top_name = top_var ? top_var->var_decl.name : NULL;
                Token *ret_name = n->ret.expr->ident.name;
                if (top_name && ret_name &&
                    top_name->len == ret_name->len &&
                    memcmp(top_name->loc, ret_name->loc, top_name->len) == 0) {
                    moved = true;
                }
            }

            int target = moved
                ? find_return_target_from(g_cf.nlive - 1)
                : return_target();
            char buf[32];
            const char *lbl;
            if (target >= 0) {
                snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
                lbl = buf;
            } else {
                lbl = "__SF_epilogue";
            }
            if (n->ret.expr) {
                fputs("__SF_RETURN(", stdout);
                emit_expr(n->ret.expr);
                fprintf(stdout, ", %s);\n", lbl);
            } else {
                fprintf(stdout, "__SF_RETURN_VOID(%s);\n", lbl);
            }
        } else {
            fputs("return", stdout);
            if (n->ret.expr) {
                fputc(' ', stdout);
                emit_expr(n->ret.expr);
            }
            fputs(";\n", stdout);
        }
        return;
    case ND_EXPR_STMT:
        if (n->expr_stmt.expr)
            emit_expr(n->expr_stmt.expr);
        fputs(";\n", stdout);
        return;
    case ND_NULL_STMT:
        fputs(";\n", stdout);
        return;
    case ND_VAR_DECL:
        emit_var_decl_inner(n);
        fputs(";\n", stdout);
        /* Direct-init 'T x(args)' lowers to a ctor call right
         * after the declaration. The class type's tag determines
         * the mangled name (mangle_class_ctor → sf__T__ctor under
         * the human scheme); first arg is &name, the rest are the
         * user args. */
        if (n->var_decl.has_ctor_init && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_STRUCT && n->var_decl.name) {
            emit_indent();
            mangle_class_ctor(n->var_decl.ty);
            fprintf(stdout, "(&%.*s",
                    n->var_decl.name->len, n->var_decl.name->loc);
            for (int i = 0; i < n->var_decl.ctor_nargs; i++) {
                fputs(", ", stdout);
                emit_expr(n->var_decl.ctor_args[i]);
            }
            fputs(");\n", stdout);
        }
        /* Default-init 'Foo a;' (no init, no parens) calls the
         * class's user-declared default ctor when one exists.
         * Without a default ctor we leave the storage uninitialized
         * — same as C semantics, and matching what C++ does for
         * trivially-default-constructible types. */
        else if (!n->var_decl.init && !n->var_decl.has_ctor_init &&
                 n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
                 n->var_decl.ty->has_default_ctor && n->var_decl.name) {
            emit_indent();
            mangle_class_ctor(n->var_decl.ty);
            fprintf(stdout, "(&%.*s);\n",
                    n->var_decl.name->len, n->var_decl.name->loc);
        }
        return;
    case ND_IF:
        if (expr_has_class_temp(n->if_.cond)) {
            /* Slice D-cond: condition contains a class temp that
             * must be destroyed BEFORE the then/else branches run
             * (N4659 §6.7.7 [class.temporary]/4 — temps die at end
             * of full-expression, which here is the cond).
             *
             * Lowering: declare a synthetic int OUTSIDE the if,
             * compute the cond into it inside a mini-block, fire
             * temp dtors via the cleanup chain, then emit the
             * actual 'if (synth)' using the captured value.
             *
             *   int __SF_cond_N;
             *   {
             *       struct T __SF_temp_M;
             *       T_ctor(&__SF_temp_M, ...);
             *       __SF_cond_N = (__SF_temp_M.v == 7);
             *   __SF_cleanup_M: ;
             *       T_dtor(&__SF_temp_M);
             *       __SF_CHAIN_ANY(<parent>);
             *   }
             *   if (__SF_cond_N) ... else ...
             */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);

            /* Synth decl at the current indent (emit_block already
             * emitted indent for us). Type is int — cond is
             * implicitly converted to bool/int for the test, and
             * int is always assignable from any scalar cond. */
            fprintf(stdout, "int %s;\n", cond_name);

            /* Open mini-block */
            emit_indent();
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;

            /* Hoist temps from the cond (inside the mini-block,
             * so their decls are emitted at the mini-block's
             * indent level). */
            hoist_temps_in_expr(n->if_.cond);

            /* Assign the cond into the synthetic. */
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->if_.cond);
            fputs(";\n", stdout);

            /* Cleanup chain — fires temp dtors before exiting
             * the mini-block. No-op if no CL_VARs were pushed
             * (trivially-destructible temp). */
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;

            g_indent--;
            emit_indent();
            fputs("}\n", stdout);

            /* Now emit the actual if using the synthetic. */
            emit_indent();
            fprintf(stdout, "if (%s) ", cond_name);
            emit_stmt(n->if_.then_);
            if (n->if_.else_) {
                emit_indent();
                fputs("else ", stdout);
                emit_stmt(n->if_.else_);
            }
            return;
        }
        fputs("if (", stdout);
        emit_expr(n->if_.cond);
        fputs(") ", stdout);
        emit_stmt(n->if_.then_);
        if (n->if_.else_) {
            emit_indent();
            fputs("else ", stdout);
            emit_stmt(n->if_.else_);
        }
        return;
    case ND_WHILE: {
        Node *body = n->while_.body;
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = expr_has_class_temp(n->while_.cond);
        if (!body_wrap && !cond_wrap) {
            /* Simple natural form — no cleanups anywhere. */
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        /* Slice C: when the body has cleanups, push a CL_LOOP marker
         * so body break/continue chains terminate at the loop's
         * synthetic boundary labels. */
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
        }
        if (cond_wrap) {
            /* Slice D-cond: cond contains a class temp. Lower to
             *   while (1) {
             *       int __SF_cond_<n>;
             *       { hoist + assign + cleanup }
             *       if (!__SF_cond_<n>) break/goto __SF_loop_break_<m>;
             *       <body>
             *       __SF_loop_cont_<m>: ;   (only if body_wrap)
             *   }
             * The cond's temp dtors fire each iteration before the
             * test, matching C++'s end-of-full-expression timing. */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            fputs("while (1) {\n", stdout);
            g_indent++;
            emit_indent();
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->while_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->while_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            emit_indent();
            if (body_wrap)
                fprintf(stdout, "if (!%s) goto __SF_loop_break_%d;\n",
                        cond_name, brk);
            else
                fprintf(stdout, "if (!%s) break;\n", cond_name);
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
        } else {
            /* body_wrap only — natural while preserved (Slice C). */
            fputs("while (", stdout);
            emit_expr(n->while_.cond);
            fputs(") {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_indent();
            fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
        }
        if (body_wrap) {
            g_cf.nlive--;  /* pop CL_LOOP marker */
            emit_indent();
            fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        }
        return;
    }
    case ND_DO: {
        Node *body = n->do_.body;
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = expr_has_class_temp(n->do_.cond);
        if (!body_wrap && !cond_wrap) {
            fputs("do ", stdout);
            emit_stmt(body);
            emit_indent();
            fputs("while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
        }
        if (cond_wrap) {
            /* Slice D-cond for do-while: the synth must be declared
             * OUTSIDE the do body because the 'while (synth)' is
             * outside the body's brace pair. The cond eval mini-
             * block lives at the END of each body iteration.
             *
             *   int __SF_cond_<n>;
             *   do {
             *       <body>
             *       __SF_loop_cont_<m>: ;       (only if body_wrap)
             *       { hoist + assign + cleanup }
             *   } while (__SF_cond_<n>);
             *   __SF_loop_break_<m>: ;          (only if body_wrap) */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            fputs("do {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            emit_indent();
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->do_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->do_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            g_indent--;
            emit_indent();
            fprintf(stdout, "} while (%s);\n", cond_name);
        } else {
            /* body_wrap only — natural do-while preserved (Slice C). */
            fputs("do {\n", stdout);
            g_indent++;
            emit_indent();
            emit_stmt(body);
            emit_indent();
            fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            g_indent--;
            emit_indent();
            fputs("} while (", stdout);
            emit_expr(n->do_.cond);
            fputs(");\n", stdout);
        }
        if (body_wrap) {
            g_cf.nlive--;
            emit_indent();
            fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        }
        return;
    }
    case ND_FOR: {
        Node *body = n->for_.body;
        bool body_wrap = g_cf.func_has_cleanups && subtree_has_cleanups(body);
        bool cond_wrap = n->for_.cond && expr_has_class_temp(n->for_.cond);
        if (!body_wrap && !cond_wrap) {
            fputs("for (", stdout);
            if (n->for_.init) {
                Node *init = n->for_.init;
                if (init->kind == ND_VAR_DECL) {
                    emit_var_decl_inner(init);
                } else if (init->kind == ND_EXPR_STMT) {
                    emit_expr(init->expr_stmt.expr);
                }
            }
            fputs("; ", stdout);
            if (n->for_.cond) emit_expr(n->for_.cond);
            fputs("; ", stdout);
            if (n->for_.inc)  emit_expr(n->for_.inc);
            fputs(") ", stdout);
            emit_stmt(body);
            return;
        }
        int brk = -1, cnt = -1;
        if (body_wrap) {
            brk = g_cf.next_label_id++;
            cnt = g_cf.next_label_id++;
            if (g_cf.nlive < CLEANUP_LIVE_MAX) {
                g_cf.live[g_cf.nlive].kind = CL_LOOP;
                g_cf.live[g_cf.nlive].label_id = brk;
                g_cf.live[g_cf.nlive].cont_label_id = cnt;
                g_cf.live[g_cf.nlive].var_decl = NULL;
                g_cf.nlive++;
            }
        }
        if (cond_wrap) {
            /* Slice D-cond for for-loop: lower to a while(1) form
             * because the for-loop's cond slot can't hold the
             * mini-block. Init runs once in a wrapping block;
             * inc runs at the bottom of each iteration; cond runs
             * via mini-block at the top of each iteration.
             *
             *   {
             *       init;
             *       while (1) {
             *           int __SF_cond_<n>;
             *           { hoist + assign + cleanup }
             *           if (!__SF_cond_<n>) break/goto loop_break;
             *           <body>
             *           __SF_loop_cont_<m>: ;
             *           inc;
             *       }
             *       __SF_loop_break_<m>: ;
             *   }
             *
             * Init/inc temps are NOT handled here; if they have
             * class temps the lowering will be slightly wrong
             * (extended-lifetime). Documented limitation. */
            int cond_id = g_cf.next_label_id++;
            char cond_name[24];
            snprintf(cond_name, sizeof(cond_name), "__SF_cond_%d", cond_id);
            fputs("{\n", stdout);
            g_indent++;
            if (n->for_.init) {
                emit_indent();
                Node *init = n->for_.init;
                if (init->kind == ND_VAR_DECL) {
                    emit_var_decl_inner(init);
                    fputs(";\n", stdout);
                } else if (init->kind == ND_EXPR_STMT) {
                    emit_expr(init->expr_stmt.expr);
                    fputs(";\n", stdout);
                }
            }
            emit_indent();
            fputs("while (1) {\n", stdout);
            g_indent++;
            emit_indent();
            fprintf(stdout, "int %s;\n", cond_name);
            emit_indent();
            fputs("{\n", stdout);
            g_indent++;
            int saved_nlive = g_cf.nlive;
            hoist_temps_in_expr(n->for_.cond);
            emit_indent();
            fprintf(stdout, "%s = ", cond_name);
            emit_expr(n->for_.cond);
            fputs(";\n", stdout);
            emit_cleanup_chain_for_added(saved_nlive);
            g_cf.nlive = saved_nlive;
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            emit_indent();
            if (body_wrap)
                fprintf(stdout, "if (!%s) goto __SF_loop_break_%d;\n",
                        cond_name, brk);
            else
                fprintf(stdout, "if (!%s) break;\n", cond_name);
            emit_indent();
            emit_stmt(body);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
            }
            if (n->for_.inc) {
                emit_indent();
                emit_expr(n->for_.inc);
                fputs(";\n", stdout);
            }
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            if (body_wrap) {
                emit_indent();
                fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
            }
            g_indent--;
            emit_indent();
            fputs("}\n", stdout);
            if (body_wrap) g_cf.nlive--;
            return;
        }
        /* body_wrap only — natural for preserved (Slice C). */
        fputs("for (", stdout);
        if (n->for_.init) {
            Node *init = n->for_.init;
            if (init->kind == ND_VAR_DECL) {
                emit_var_decl_inner(init);
            } else if (init->kind == ND_EXPR_STMT) {
                emit_expr(init->expr_stmt.expr);
            }
        }
        fputs("; ", stdout);
        if (n->for_.cond) emit_expr(n->for_.cond);
        fputs("; ", stdout);
        if (n->for_.inc)  emit_expr(n->for_.inc);
        fputs(") {\n", stdout);
        g_indent++;
        emit_indent();
        emit_stmt(body);
        emit_indent();
        fprintf(stdout, "__SF_loop_cont_%d: ;\n", cnt);
        g_indent--;
        emit_indent();
        fputs("}\n", stdout);
        g_cf.nlive--;
        emit_indent();
        fprintf(stdout, "__SF_loop_break_%d: ;\n", brk);
        return;
    }
    case ND_GOTO: {
        /* User goto. When CL_VARs are live, fire their dtors INLINE
         * (in reverse declaration order) before the actual jump.
         * The dtors are duplicated relative to the cleanup chain
         * but on a different control path — the chain's labels are
         * reached via fall-through which doesn't happen here.
         *
         * Conservative approximation: fire ALL live CL_VARs. This
         * is correct when the label is at function scope (most
         * common). For labels in a same-or-nested scope (rare and
         * C++ forbids gotos that cross initialized declarations
         * anyway), this would over-destroy. A future refinement
         * would consult sema-level label-depth tracking.
         *
         * Wrapped in '{ }' so an unbraced 'if (cond) goto X;'
         * stays correct — without the braces the if would only
         * guard the first dtor and the goto would run
         * unconditionally. Same pattern as __SF_RETURN. */
        fputc('{', stdout);
        for (int i = g_cf.nlive - 1; i >= 0; i--) {
            CleanupEntry *e = &g_cf.live[i];
            if (e->kind != CL_VAR) continue;
            Node *v = e->var_decl;
            fputc(' ', stdout);
            if (v->codegen_temp_name) {
                mangle_class_dtor(v->resolved_type);
                fprintf(stdout, "(&%s);", v->codegen_temp_name);
            } else {
                mangle_class_dtor(v->var_decl.ty);
                fprintf(stdout, "(&%.*s);",
                        v->var_decl.name->len, v->var_decl.name->loc);
            }
        }
        if (n->goto_.label)
            fprintf(stdout, " goto %.*s; }\n",
                    n->goto_.label->len, n->goto_.label->loc);
        else
            fputs(" goto __SF_unknown; }\n", stdout);
        return;
    }
    case ND_LABEL:
        if (n->label.label)
            fprintf(stdout, "%.*s: ",
                    n->label.label->len, n->label.label->loc);
        if (n->label.stmt)
            emit_stmt(n->label.stmt);
        else
            fputs(";\n", stdout);
        return;
    case ND_BREAK:
        if (break_needs_cleanup()) {
            /* Slice C: walk the cleanup chain before exiting the
             * loop. Topmost live entry is a CL_VAR — its label is
             * the next chain target; the chain itself terminates
             * at __SF_loop_break_<n> for the innermost CL_LOOP. */
            int target = break_target();
            char buf[32];
            snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
            fprintf(stdout, "__SF_BREAK(%s);\n", buf);
        } else {
            fputs("break;\n", stdout);
        }
        return;
    case ND_CONTINUE:
        if (break_needs_cleanup()) {
            /* Same shape as break — at this point top-of-stack must
             * be a CL_VAR (otherwise break_needs_cleanup() would be
             * false), so cont's first chain target is that var's
             * cleanup label. */
            int target = cont_target();
            char buf[32];
            snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
            fprintf(stdout, "__SF_CONT(%s);\n", buf);
        } else {
            fputs("continue;\n", stdout);
        }
        return;
    default:
        fputs("/* stmt */;\n", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Top-level emission                                                 */
/* ------------------------------------------------------------------ */

/* Reset the per-function dtor lowering state and decide whether the
 * function needs the cleanup machinery at all. Called at the entry
 * to every function/method emission. */
static void cf_begin_function(Node *func) {
    g_cf.next_label_id = 0;
    g_cf.nlive = 0;
    g_cf.func_has_cleanups = func && func->func.body &&
                             subtree_has_cleanups(func->func.body);
}

/* When emit_class_def is iterating its method members it sets this
 * to the class def node — ctor/dtor body emission can then consult
 * it to walk the class's members in declaration order (which the
 * hash-bucketed class_region doesn't preserve). NULL outside the
 * class member loop. (Declaration is at the top of the file —
 * forward-needed by ND_IDENT and ND_CALL emission for inherited-
 * member access rewriting.) */

/* For a ctor with mem-initializers and/or class members needing
 * default construction, emit member ctor calls at the top of the
 * body — N4659 §15.6.2/13.3, in DECLARATION order. Members listed
 * in the mem-init-list use the user's args; class members not
 * listed but with non-trivial default ctors get the default ctor
 * call; non-class members listed get a plain assignment. */
static void emit_ctor_member_inits(Node *func) {
    if (!func->func.is_constructor) return;
    if (!g_current_class_def) return;
    Node *cdef = g_current_class_def;
    /* Base subobject construction — N4659 §15.6.2/13 [class.base.init]:
     * "...non-static data members are initialized in the order
     *  they were declared in the class definition (again regardless
     *  of the order of the mem-initializers)" — and bases come first.
     * For each direct base, call its default ctor if it has one.
     * (Mem-init-list base-class entries with explicit args are NOT
     * yet handled — that needs a richer mem-init parse to recognise
     * 'Base(args)' versus 'member(args)'.) */
    if (cdef->class_def.ty) {
        Type *cty = cdef->class_def.ty;
        int nb = class_nbases(cty);
        for (int b = 0; b < nb; b++) {
            Type *base = class_base(cty, b);
            if (!base || !base->has_default_ctor) continue;
            emit_indent();
            mangle_class_ctor(base);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
    }
    /* Vptr install — N4659 §15.6.2/1 [class.base.init]: a polymorphic
     * class's virtual functions become callable through the object
     * once construction is *complete*. We approximate by setting the
     * vptr at the *start* of the ctor (after base ctors so it
     * overrides any vptr the base installed, before member inits
     * and the user body). */
    if (cdef->class_def.ty && cdef->class_def.ty->has_virtual_methods) {
        emit_indent();
        fputs("this->__sf_vptr = &", stdout);
        mangle_class_vtable_instance(cdef->class_def.ty);
        fputs(";\n", stdout);
    }
    for (int i = 0; i < cdef->class_def.nmembers; i++) {
        Node *m = cdef->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Type *mty = m->var_decl.ty;
        if (!mty) continue;
        if (!m->var_decl.name) continue;
        /* Skip member functions. */
        if (mty->kind == TY_FUNC) continue;

        /* Look up this member in the user's mem-init-list. */
        MemInit *found = NULL;
        for (int k = 0; k < func->func.n_mem_inits; k++) {
            MemInit *mi = &func->func.mem_inits[k];
            if (mi->name && mi->name->len == m->var_decl.name->len &&
                memcmp(mi->name->loc, m->var_decl.name->loc,
                       mi->name->len) == 0) {
                found = mi;
                break;
            }
        }

        if (mty->kind == TY_STRUCT) {
            /* Class member: ctor call with the user's args, or
             * default ctor if not listed. */
            if (found) {
                emit_indent();
                mangle_class_ctor(mty);
                fprintf(stdout, "(&this->%.*s",
                        m->var_decl.name->len, m->var_decl.name->loc);
                for (int a = 0; a < found->nargs; a++) {
                    fputs(", ", stdout);
                    emit_expr(found->args[a]);
                }
                fputs(");\n", stdout);
            } else if (mty->has_default_ctor) {
                emit_indent();
                mangle_class_ctor(mty);
                fprintf(stdout, "(&this->%.*s);\n",
                        m->var_decl.name->len, m->var_decl.name->loc);
            }
            /* else: trivially-default-constructible member, nothing to do. */
        } else {
            /* Non-class member (scalar / pointer / array): a listed
             * entry like ': x(7)' lowers to a plain assignment
             * 'this->x = 7;'. Multi-arg forms are not meaningful for
             * a scalar and would be a parse error in C++; we ignore
             * the second-and-later args. Non-listed scalar members
             * are left default-initialized (= uninitialized) per C
             * semantics. */
            if (found && found->nargs >= 1) {
                emit_indent();
                fprintf(stdout, "this->%.*s = ",
                        m->var_decl.name->len, m->var_decl.name->loc);
                emit_expr(found->args[0]);
                fputs(";\n", stdout);
            }
        }
    }
}

/* Emit the body of a function with cleanup-aware wrapping when the
 * function has any non-trivial locals: declare __retval/__unwind at
 * the top, run the body, then __epilogue: return __retval; */
static void emit_func_body(Node *func) {
    if (!func->func.body) { fputs(";\n", stdout); return; }
    /* Ctors in any class need the wrapped form so emit_ctor_member_inits
     * can run — that's where mem-init list AND vptr install live.
     * Non-ctors only need the wrap when they have cleanups. */
    bool has_member_inits = func->func.is_constructor &&
                            g_current_class_def != NULL;
    if (!g_cf.func_has_cleanups && !has_member_inits) {
        emit_block(func->func.body);
        return;
    }
    /* Wrap: open a brace, expand __SF_PROLOGUE (declares __SF_retval
     * and __SF_unwind), emit member-init calls if this is a ctor,
     * emit the user body (its own ND_BLOCK prints its { }), then
     * __SF_EPILOGUE (label + return). */
    fputs("{\n", stdout);
    g_indent++;
    bool void_ret = func->func.ret_ty && func->func.ret_ty->kind == TY_VOID;
    if (g_cf.func_has_cleanups) {
        emit_indent();
        if (void_ret) {
            fputs("__SF_PROLOGUE_VOID;\n", stdout);
        } else {
            fputs("__SF_PROLOGUE(", stdout);
            emit_type(func->func.ret_ty);
            fputs(");\n", stdout);
        }
    }
    if (has_member_inits) emit_ctor_member_inits(func);
    /* Skip emitting the user body block entirely when it has zero
     * statements — the wrapper's own braces are already emitted, so
     * dropping the empty inner '{ }' is purely cosmetic and keeps
     * generated ctors/dtors readable. emit_block's cleanup-chain
     * bookkeeping is a no-op for a stmt-less body anyway. */
    Node *body = func->func.body;
    bool body_empty = body && body->kind == ND_BLOCK && body->block.nstmts == 0;
    if (!body_empty) {
        emit_indent();
        emit_block(body);
    }
    if (g_cf.func_has_cleanups) {
        emit_indent();
        fputs(void_ret ? "__SF_EPILOGUE_VOID;\n" : "__SF_EPILOGUE;\n", stdout);
    }
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

static void emit_func_def(Node *n) {
    emit_source_comment(n->tok);
    cf_begin_function(n);
    emit_type(n->func.ret_ty);
    fputc(' ', stdout);
    if (n->func.name)
        fprintf(stdout, "%.*s", n->func.name->len, n->func.name->loc);
    fputc('(', stdout);
    if (n->func.nparams == 0) {
        fputs("void", stdout);
    } else {
        for (int i = 0; i < n->func.nparams; i++) {
            if (i > 0) fputs(", ", stdout);
            Node *p = n->func.params[i];
            emit_type(p->param.ty);
            if (p->param.name)
                fprintf(stdout, " %.*s",
                        p->param.name->len, p->param.name->loc);
            else
                fprintf(stdout, " __sf_unused_%d", i);
        }
    }
    fputs(") ", stdout);
    emit_func_body(n);
}

/* Emit just the signature of a method as a mangled free function.
 * Used for forward declarations so methods can call each other in
 * any order regardless of source ordering inside the class body.
 *
 * 'class_type' carries class_region for namespace walking; the bare
 * tag alone is not enough because two classes named the same in
 * different namespaces would collide. */
/*
 * Emit a mangled operator method name. 'operator[]' → '__subscript',
 * 'operator=' → '__assign', 'operator==' → '__eq', etc. The name
 * token for operator functions is the 'operator' keyword; the
 * actual operator is the next token(s) in the source. We look at
 * the token following 'operator' to determine the suffix.
 *
 * Falls back to '__operator' for unrecognised operators.
 */
static void emit_operator_method_name(Type *class_type, Token *name) {
    /* The 'operator' keyword token — the operator symbol follows
     * in the source. We can peek at the bytes after the token. */
    const char *after = name->loc + name->len;
    /* Skip whitespace */
    while (*after == ' ' || *after == '\t') after++;

    const char *suffix = "__operator";
    if (after[0] == '[')       suffix = "__subscript";
    else if (after[0] == '(' && after[1] == ')') suffix = "__call";
    else if (after[0] == '=' && after[1] == '=') suffix = "__eq";
    else if (after[0] == '!' && after[1] == '=') suffix = "__ne";
    else if (after[0] == '<' && after[1] == '=') suffix = "__le";
    else if (after[0] == '>' && after[1] == '=') suffix = "__ge";
    else if (after[0] == '<' && after[1] != '<') suffix = "__lt";
    else if (after[0] == '>' && after[1] != '>') suffix = "__gt";
    else if (after[0] == '+' && after[1] == '=') suffix = "__plus_assign";
    else if (after[0] == '-' && after[1] == '=') suffix = "__minus_assign";
    else if (after[0] == '*' && after[1] == '=') suffix = "__mul_assign";
    else if (after[0] == '/' && after[1] == '=') suffix = "__div_assign";
    else if (after[0] == '+' && after[1] == '+') suffix = "__incr";
    else if (after[0] == '-' && after[1] == '-') suffix = "__decr";
    else if (after[0] == '+')  suffix = "__plus";
    else if (after[0] == '-' && after[1] == '>') suffix = "__arrow";
    else if (after[0] == '-')  suffix = "__minus";
    else if (after[0] == '*')  suffix = "__deref";
    else if (after[0] == '/')  suffix = "__div";
    else if (after[0] == '%')  suffix = "__mod";
    else if (after[0] == '&' && after[1] == '&') suffix = "__land";
    else if (after[0] == '|' && after[1] == '|') suffix = "__lor";
    else if (after[0] == '&')  suffix = "__bitand";
    else if (after[0] == '|')  suffix = "__bitor";
    else if (after[0] == '^')  suffix = "__xor";
    else if (after[0] == '~')  suffix = "__compl";
    else if (after[0] == '!')  suffix = "__not";
    else if (after[0] == '<' && after[1] == '<') suffix = "__lshift";
    else if (after[0] == '>' && after[1] == '>') suffix = "__rshift";
    else if (after[0] == '=')  suffix = "__assign";

    mangle_class_tag(class_type);
    fputs(suffix, stdout);
}

/*
 * Is this function name an operator keyword?
 */
static bool is_operator_name(Token *name) {
    return name && name->kind == TK_KW_OPERATOR;
}

static void emit_method_signature(Node *func, Type *class_type) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    /* Class methods (in-class definitions are implicitly inline)
     * and dtor body functions get __SF_INLINE so multi-TU compilation
     * dedupes via weak symbols. See docs/inline_and_dedup.md. */
    fputs("__SF_INLINE ", stdout);
    emit_type(func->func.ret_ty);
    fputc(' ', stdout);
    if (func->func.is_destructor) {
        /* The user's dtor body is emitted as Class__dtor_body — the
         * '_body' suffix names what it actually contains: just the
         * user-written body, with no member-dtor chain. The
         * synthesized Class__dtor wrapper (built by emit_class_def)
         * calls Class__dtor_body first and then chains into member
         * dtor calls. Every CALLER of a class dtor still emits the
         * unsuffixed Class__dtor name — they hit the wrapper, not
         * the body function directly. */
        mangle_class_dtor_body(class_type);
    } else if (func->func.is_constructor) {
        /* Constructors mangle as Class__ctor. Single-overload only
         * for now — multiple ctors of the same class would collide
         * here and need a per-overload disambiguator (Itanium uses
         * a parameter-type-encoded suffix; we'll add something
         * similar when we tackle overloading). */
        mangle_class_ctor(class_type);
    } else if (is_operator_name(func->func.name)) {
        emit_operator_method_name(class_type, func->func.name);
    } else {
        mangle_class_method(class_type, func->func.name);
    }
    fputc('(', stdout);
    fputs("struct ", stdout);
    mangle_class_tag(class_type);
    fputs(" *this", stdout);
    for (int i = 0; i < func->func.nparams; i++) {
        fputs(", ", stdout);
        Node *p = func->func.params[i];
        emit_type(p->param.ty);
        if (p->param.name)
            fprintf(stdout, " %.*s",
                    p->param.name->len, p->param.name->loc);
        else
            /* C requires named parameters in function definitions.
             * C++ allows unnamed params; synthesise a name. */
            fprintf(stdout, " __sf_unused_%d", i);
    }
    fputc(')', stdout);
}

/* Emit a method definition as a free C function with a mangled name
 * and an explicit 'this' parameter.
 *
 *   struct Point { int sum() { return x + y; } };
 * becomes
 *   int Point_sum(struct Point *this) { return this->x + this->y; }
 *
 * The 'this->' rewrite happens at the ident level — visit_ident set
 * implicit_this on each member reference, and emit_expr emits the
 * prefix when it sees the flag. */
static void emit_method_as_free_fn(Node *func, Type *class_type) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    emit_source_comment(func->tok);
    cf_begin_function(func);
    emit_method_signature(func, class_type);
    fputc(' ', stdout);
    emit_func_body(func);
}

static void emit_class_def(Node *n) {
    /* Emit a C struct from the parsed class definition.
     * When g_emit_phase == PHASE_STRUCTS: emit only the struct body
     *   and method forward declarations.
     * When g_emit_phase == PHASE_METHODS: emit method bodies, vtable,
     *   synthesized ctors/dtors.
     * When g_emit_phase == 0: emit everything (single-pass mode). */
    Type *class_type = n->class_def.ty;

    /* Two-phase emission support: in PHASE_STRUCTS, emit only
     * the struct definition + method forward declarations. In
     * PHASE_METHODS, skip the struct (already emitted) and emit
     * only method bodies, vtable, synthesized ctors/dtors. */
    if (g_emit_phase == PHASE_METHODS) goto methods_phase;

    /* Skip if already emitted (dedup for dependency-driven emit). */
    if (class_type && class_type->codegen_emitted) return;
    if (class_type) class_type->codegen_emitted = true;

    /* Emit struct dependencies first: any by-value struct/union
     * member whose type has a class_def must be emitted before
     * this struct. This handles the line-map.h pattern where
     * a union contains by-value struct members. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_VAR_DECL) continue;
        Type *mty = m->var_decl.ty;
        if (!mty) continue;
        if ((mty->kind == TY_STRUCT || mty->kind == TY_UNION) &&
            mty->class_def && !mty->codegen_emitted)
            emit_class_def(mty->class_def);
    }
    /* Also emit base class definitions first. */
    if (class_type && class_type->class_region) {
        for (int i = 0; i < class_type->class_region->nbases; i++) {
            DeclarativeRegion *br = class_type->class_region->bases[i];
            if (br && br->owner_type && br->owner_type->class_def &&
                !br->owner_type->codegen_emitted)
                emit_class_def(br->owner_type->class_def);
        }
    }

    emit_source_comment(n->tok);

    /* For polymorphic classes (any virtual method), forward-declare
     * the vtable struct so we can place a vptr field at offset 0 of
     * the layout. The vtable struct itself is defined later, after
     * all method forward-declarations are out, so it can name them
     * in its function-pointer slots. */
    bool poly = class_type && class_type->has_virtual_methods;
    if (poly) {
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(";\n", stdout);
    }

    fputs((class_type && class_type->kind == TY_UNION) ? "union " : "struct ",
          stdout);
    if (class_type)
        emit_mangled_class_tag(class_type);
    else if (n->class_def.tag)
        fprintf(stdout, "%.*s",
                n->class_def.tag->len, n->class_def.tag->loc);
    fputc(' ', stdout);
    fputs("{\n", stdout);
    g_indent++;
    /* Base subobjects — N4659 §11 [class.derived] / §6.7 [class.layout].
     * For non-virtual single (or multiple) inheritance we embed each
     * direct base as a struct field at the head of the layout, in
     * declaration order. The base field is named '__sf_base' (or
     * '__sf_base<N>' for the second-and-later base) and is itself a
     * 'struct sf__<Base>'. Inherited member access lowers to
     *   this->__sf_base.x
     * via the rewriting in emit_member_via_base / emit_ident.
     *
     * Placing the base FIRST in the layout (and the polymorphic vptr
     * inside the base, not duplicated in the derived) means a
     * 'Derived*' is layout-compatible with 'Base*' for casts — the
     * is-a relationship works at the C struct level too. */
    int nb = class_nbases(class_type);
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(class_type, b);
        if (!base) continue;
        emit_indent();
        fputs("struct ", stdout);
        mangle_class_tag(base);
        if (b == 0) fputs(" __sf_base;\n", stdout);
        else        fprintf(stdout, " __sf_base%d;\n", b);
    }
    /* Vptr at offset 0 — N4659 §13.3 [class.virtual]. The standard
     * doesn't mandate offset 0 but every Itanium-style ABI puts it
     * there for the simple non-multi-inheritance case, and so do we.
     *
     * If the class inherits from a polymorphic base, the vptr lives
     * inside the base subobject and we DO NOT add another vptr here
     * — that would offset the base's layout and break the is-a
     * compatibility. */
    bool has_poly_base = false;
    for (int b = 0; b < nb; b++) {
        Type *base = class_base(class_type, b);
        if (base && base->has_virtual_methods) { has_poly_base = true; break; }
    }
    if (poly && !has_poly_base) {
        emit_indent();
        fputs("const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" *__sf_vptr;\n", stdout);
    }
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        /* Flat blocks from comma-separated declarations:
         * 'size_t precision, char_precision;' → ND_BLOCK(is_flat)
         * containing individual ND_VAR_DECLs. Flatten them. */
        if (m->kind == ND_BLOCK && m->block.is_flat) {
            for (int j = 0; j < m->block.nstmts; j++) {
                Node *s = m->block.stmts[j];
                if (!s || s->kind != ND_VAR_DECL) continue;
                if (s->var_decl.ty && s->var_decl.ty->kind == TY_FUNC) continue;
                emit_indent();
                emit_var_decl_inner(s);
                fputs(";\n", stdout);
            }
            continue;
        }
        if (m->kind != ND_VAR_DECL) continue;
        /* Member functions (ND_VAR_DECL with TY_FUNC) are forward-declared
         * separately — skip them here. BUT: function POINTER members
         * (typedefs like 'convert_f func;' where the typedef lost the
         * pointer level) should be emitted as 'ret_type (*name)(params)'.
         *
         * Heuristic: if the class_region has this name registered as a
         * function (TY_FUNC entity), it's a method declaration — skip.
         * Otherwise it's a function pointer data member — emit. */
        if (m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC) {
            bool is_method = false;
            if (m->var_decl.is_constructor || m->var_decl.is_destructor ||
                m->var_decl.is_virtual)
                is_method = true;
            /* If the class has any ND_FUNC_DEF members (actual method
             * bodies), TY_FUNC members without ctor/dtor/virtual flags
             * are method declarations. If the class has NO ND_FUNC_DEF
             * members (plain C struct), TY_FUNC members are function
             * pointer fields from typedefs. */
            if (!is_method) {
                for (int k = 0; k < n->class_def.nmembers; k++) {
                    Node *mk = n->class_def.members[k];
                    if (mk && mk->kind == ND_FUNC_DEF) {
                        is_method = true;
                        break;
                    }
                }
            }
            if (is_method) continue;
            /* Function pointer member: emit as 'ret (*name)(params)' */
            if (!m->var_decl.name) continue;
            emit_indent();
            Type *fty = m->var_decl.ty;
            emit_type(fty->ret);
            fprintf(stdout, " (*%.*s)(",
                    m->var_decl.name->len, m->var_decl.name->loc);
            for (int j = 0; j < fty->nparams; j++) {
                if (j > 0) fputs(", ", stdout);
                emit_type(fty->params[j]);
            }
            if (fty->nparams == 0) fputs("void", stdout);
            fputs(");\n", stdout);
            continue;
        }
        emit_indent();
        emit_var_decl_inner(m);
        fputs(";\n", stdout);
    }
    g_indent--;
    fputs("};\n", stdout);

    /* Forward-declare every method first, so they can call each
     * other regardless of source order inside the class body, and so
     * out-of-class definitions ('int Foo::bar() {}') see a prior
     * declaration of the mangled name. Both in-class definitions
     * (ND_FUNC_DEF) and pure declarations (ND_VAR_DECL with TY_FUNC)
     * count.
     *
     * Dtor handling: a user-declared dtor with non-empty body lowers
     * to mangle_class_dtor_body (sf__Class__dtor_body), forward-
     * declared and emitted by the normal method path. The
     * mangle_class_dtor wrapper (sf__Class__dtor, synthesized
     * below) is forward-declared separately when
     * class_type->has_dtor is true — that flag may be set by a
     * user dtor, by a non-trivially-destructible member, OR by a
     * non-trivially-destructible direct base — so the wrapper
     * exists even when no user dtor was written. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        /* Skip dtors with empty bodies — no _dtor_body emission.
         * The class wrapper handles members directly. has_dtor may
         * still be true (because of members), so the wrapper is
         * emitted separately below. */
        if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
            Node *body = m->func.body;
            bool empty = body && body->kind == ND_BLOCK &&
                         body->block.nstmts == 0;
            if (empty) continue;
            if (!class_type || !class_type->has_dtor) continue;
        }
        if (m->kind == ND_FUNC_DEF && class_type) {
            emit_source_comment(m->tok);
            emit_method_signature(m, class_type);
            fputs(";\n", stdout);
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC && m->var_decl.name &&
                   class_type) {
            /* Synthesise an emit_method_signature-like header from
             * the var-decl's type. Ctor/dtor declarations route
             * through mangle_class_ctor / mangle_class_dtor_body
             * instead of the regular mangle_class_method form. */
            emit_source_comment(m->tok);
            Type *fty = m->var_decl.ty;
            if (m->var_decl.is_destructor) {
                /* In-class declaration of a dtor whose body is
                 * defined out-of-class (Foo::~Foo() { ... }). The
                 * sf__Class__dtor wrapper (synthesized below) is
                 * forward-declared separately, but we DO need a
                 * forward decl for sf__Class__dtor_body so the
                 * wrapper can call it. */
                fputs("__SF_INLINE void ", stdout);
                mangle_class_dtor_body(class_type);
                fputs("(struct ", stdout);
                mangle_class_tag(class_type);
                fputs(" *this);\n", stdout);
                continue;
            }
            fputs("__SF_INLINE ", stdout);
            emit_type(fty->ret);
            fputc(' ', stdout);
            if (m->var_decl.is_constructor) {
                mangle_class_ctor(class_type);
            } else if (is_operator_name(m->var_decl.name)) {
                emit_operator_method_name(class_type, m->var_decl.name);
            } else {
                mangle_class_method(class_type, m->var_decl.name);
            }
            fputs("(struct ", stdout);
            mangle_class_tag(class_type);
            fputs(" *this", stdout);
            for (int k = 0; k < fty->nparams; k++) {
                fputs(", ", stdout);
                emit_type(fty->params[k]);
            }
            fputs(");\n", stdout);
        }
    }

    /* Forward-declare and (later) emit the sf__Class__dtor wrapper
     * when the class is non-trivially-destructible. The wrapper
     * exists whether or not a user dtor was written. */
    Node *user_dtor = NULL;
    bool user_dtor_out_of_class = false;
    if (class_type && class_type->has_dtor) {
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
                Node *body = m->func.body;
                bool empty = body && body->kind == ND_BLOCK &&
                             body->block.nstmts == 0;
                if (!empty) user_dtor = m;
                break;
            }
            if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_destructor) {
                /* Out-of-class definition: the body lives at namespace
                 * scope as Foo::~Foo() {...}. We don't have direct
                 * access to the body here, so we can't check for
                 * emptiness — assume it has content (matches the
                 * has_dtor=true assumption in type.c). */
                user_dtor_out_of_class = true;
                break;
            }
        }
        fputs("__SF_INLINE void ", stdout);
        mangle_class_dtor(class_type);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this);\n", stdout);
    }

    if (g_emit_phase == PHASE_STRUCTS) return;
methods_phase:;

    /* Re-scan for user dtor (needed for methods phase regardless
     * of which phase we entered from). */
    Node *user_dtor_m = NULL;
    bool user_dtor_m_out_of_class = false;
    if (class_type && class_type->has_dtor) {
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            if (m->kind == ND_FUNC_DEF && m->func.is_destructor) {
                Node *body = m->func.body;
                bool empty = body && body->kind == ND_BLOCK &&
                             body->block.nstmts == 0;
                if (!empty) user_dtor_m = m;
                break;
            }
            if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_destructor) {
                user_dtor_m_out_of_class = true;
                break;
            }
        }
    }

    /* Vtable struct + static instance — N4659 §13.3 [class.virtual].
     *
     * The struct has one function-pointer slot per virtual method,
     * in declaration order. The static instance is filled with the
     * mangled method addresses; ctors install '&instance' into the
     * object's __sf_vptr field, and call sites dispatch through it.
     *
     * First-slice limitation: virtual destructors aren't yet given
     * a vtable slot — they'd require slot-routing for the dtor wrapper
     * which we'll add when we tackle delete-through-base. */
    if (class_type && class_type->has_virtual_methods) {
        /* The struct definition. */
        fputs("struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputs(" {\n", stdout);
        g_indent++;
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            bool is_virt_funcdef = (m->kind == ND_FUNC_DEF && m->func.is_virtual);
            bool is_virt_decl = (m->kind == ND_VAR_DECL && m->var_decl.is_virtual &&
                                 m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC);
            if (!is_virt_funcdef && !is_virt_decl) continue;
            /* Skip virtual destructors for the first slice (see above). */
            if (is_virt_funcdef && m->func.is_destructor) continue;
            if (is_virt_decl && m->var_decl.is_destructor) continue;

            Type *ret_ty = is_virt_funcdef ? m->func.ret_ty
                                           : m->var_decl.ty->ret;
            Token *mname = is_virt_funcdef ? m->func.name : m->var_decl.name;
            int nparams = is_virt_funcdef ? m->func.nparams
                                          : m->var_decl.ty->nparams;

            emit_indent();
            emit_type(ret_ty);
            fprintf(stdout, " (*%.*s)(struct ",
                    mname->len, mname->loc);
            mangle_class_tag(class_type);
            fputs(" *", stdout);
            if (is_virt_funcdef) {
                for (int k = 0; k < nparams; k++) {
                    fputs(", ", stdout);
                    emit_type(m->func.params[k]->param.ty);
                }
            } else {
                for (int k = 0; k < nparams; k++) {
                    fputs(", ", stdout);
                    emit_type(m->var_decl.ty->params[k]);
                }
            }
            fputs(");\n", stdout);
        }
        g_indent--;
        fputs("};\n", stdout);

        /* The static instance. Each slot points at the method's
         * mangled free-function form. The forward decls for these
         * methods were emitted above, so the names are visible here. */
        fputs("static const struct ", stdout);
        mangle_class_vtable_type(class_type);
        fputc(' ', stdout);
        mangle_class_vtable_instance(class_type);
        fputs(" = {\n", stdout);
        g_indent++;
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m) continue;
            bool is_virt_funcdef = (m->kind == ND_FUNC_DEF && m->func.is_virtual);
            bool is_virt_decl = (m->kind == ND_VAR_DECL && m->var_decl.is_virtual &&
                                 m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC);
            if (!is_virt_funcdef && !is_virt_decl) continue;
            if (is_virt_funcdef && m->func.is_destructor) continue;
            if (is_virt_decl && m->var_decl.is_destructor) continue;
            Token *mname = is_virt_funcdef ? m->func.name : m->var_decl.name;
            emit_indent();
            mangle_class_method(class_type, mname);
            fputs(",\n", stdout);
        }
        g_indent--;
        fputs("};\n", stdout);
    }

    /* Now emit each method (ND_FUNC_DEF in the member list) as a
     * separate free function. Trivial dtors are skipped to match
     * the forward-decl loop above. g_current_class_def is set so
     * ctor body emission can walk this class's members in
     * declaration order to emit member-init calls. */
    Node *saved_cdef = g_current_class_def;
    g_current_class_def = n;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m || m->kind != ND_FUNC_DEF || !class_type) continue;
        if (m->func.is_destructor) {
            Node *body = m->func.body;
            bool empty = body && body->kind == ND_BLOCK &&
                         body->block.nstmts == 0;
            if (empty || !class_type->has_dtor) continue;
        }
        emit_method_as_free_fn(m, class_type);
    }
    g_current_class_def = saved_cdef;

    /* Synthesize the sf__Class__dtor wrapper. Calls
     * sf__Class__dtor_body (if a user dtor existed) FIRST, then
     * chains into each non-trivially-destructible member's dtor in
     * REVERSE declaration order — N4659 §15.4 [class.dtor]/9.
     * After the members, chains into each base subobject's dtor,
     * also in REVERSE declaration order. */
    if (class_type && class_type->has_dtor) {
        fputs("__SF_INLINE void ", stdout);
        mangle_class_dtor(class_type);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this) {\n", stdout);
        g_indent++;
        if (user_dtor_m || user_dtor_m_out_of_class) {
            emit_indent();
            mangle_class_dtor_body(class_type);
            fputs("(this);\n", stdout);
        }
        for (int i = n->class_def.nmembers - 1; i >= 0; i--) {
            Node *m = n->class_def.members[i];
            if (!m || m->kind != ND_VAR_DECL) continue;
            if (!m->var_decl.ty || m->var_decl.ty->kind != TY_STRUCT) continue;
            if (!m->var_decl.ty->has_dtor) continue;
            if (!m->var_decl.name) continue;
            emit_indent();
            mangle_class_dtor(m->var_decl.ty);
            fprintf(stdout, "(&this->%.*s);\n",
                    m->var_decl.name->len, m->var_decl.name->loc);
        }
        /* Base subobject destruction — reverse declaration order. */
        int nb_d = class_nbases(class_type);
        for (int b = nb_d - 1; b >= 0; b--) {
            Type *base = class_base(class_type, b);
            if (!base || !base->has_dtor) continue;
            emit_indent();
            mangle_class_dtor(base);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
        g_indent--;
        fputs("}\n", stdout);
    }

    /* Synthesize a default ctor when the class has has_default_ctor
     * set but no user-declared ctor exists — N4659 §15.1 [class.ctor]/4.
     * Body just chains into each member's default ctor in declaration
     * order. Trivially-default-constructible members contribute
     * nothing (no call). */
    bool any_user_ctor = false;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        if ((m->kind == ND_FUNC_DEF && m->func.is_constructor) ||
            (m->kind == ND_VAR_DECL && m->var_decl.ty &&
             m->var_decl.ty->kind == TY_FUNC && m->var_decl.is_constructor)) {
            any_user_ctor = true;
            break;
        }
    }
    if (class_type && class_type->has_default_ctor && !any_user_ctor) {
        /* Forward decl + body for the synthesized ctor. */
        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this);\n", stdout);

        fputs("__SF_INLINE void ", stdout);
        mangle_class_ctor(class_type);
        fputs("(struct ", stdout);
        mangle_class_tag(class_type);
        fputs(" *this) {\n", stdout);
        g_indent++;
        /* Base subobject construction first — declaration order. */
        int nb_c = class_nbases(class_type);
        for (int b = 0; b < nb_c; b++) {
            Type *base = class_base(class_type, b);
            if (!base || !base->has_default_ctor) continue;
            emit_indent();
            mangle_class_ctor(base);
            fputs("(&this->", stdout);
            if (b == 0) fputs("__sf_base", stdout);
            else        fprintf(stdout, "__sf_base%d", b);
            fputs(");\n", stdout);
        }
        /* Install vptr next for polymorphic classes — see the
         * matching emit_ctor_member_inits path for the rationale. */
        if (class_type->has_virtual_methods) {
            emit_indent();
            fputs("this->__sf_vptr = &", stdout);
            mangle_class_vtable_instance(class_type);
            fputs(";\n", stdout);
        }
        for (int i = 0; i < n->class_def.nmembers; i++) {
            Node *m = n->class_def.members[i];
            if (!m || m->kind != ND_VAR_DECL) continue;
            if (!m->var_decl.ty || m->var_decl.ty->kind != TY_STRUCT) continue;
            if (!m->var_decl.ty->has_default_ctor) continue;
            if (!m->var_decl.name) continue;
            emit_indent();
            mangle_class_ctor(m->var_decl.ty);
            fprintf(stdout, "(&this->%.*s);\n",
                    m->var_decl.name->len, m->var_decl.name->loc);
        }
        g_indent--;
        fputs("}\n", stdout);
    }
}

static void emit_top_level(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC_DEF:
        /* Out-of-class method definition 'int Foo::bar() {}' was
         * tagged by the parser with the resolved class type. Emit
         * it as a mangled free function with the 'this' parameter
         * prepended.
         *
         * For ctors specifically, set g_current_class_def from the
         * class type's back-pointer so emit_ctor_member_inits can
         * walk the class members in declaration order — same
         * machinery the in-class method-emission loop already uses. */
        if (n->func.class_type && n->func.class_type->tag) {
            Node *saved = g_current_class_def;
            if (n->func.is_constructor && n->func.class_type->class_def)
                g_current_class_def = n->func.class_type->class_def;
            emit_method_as_free_fn(n, n->func.class_type);
            g_current_class_def = saved;
        } else {
            /* Dedup: inline functions from headers may be included multiple
             * times in the preprocessed output. Skip redefinitions. */
            if (func_dedup_check(n->func.name)) return;
            emit_func_def(n);
        }
        return;
    case ND_CLASS_DEF: emit_class_def(n); return;
    case ND_VAR_DECL:
        /* Bare enum definition: 'enum Color { RED, GREEN };' becomes
         * ND_VAR_DECL with type TY_ENUM and no name. Emit the enum
         * body as a C enum definition. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_ENUM &&
            n->var_decl.ty->enum_tokens && n->var_decl.ty->enum_ntokens > 0 &&
            !n->var_decl.name) {
            if (n->var_decl.ty->codegen_emitted) return;
            n->var_decl.ty->codegen_emitted = true;
            emit_source_comment(n->tok);
            fputs("enum ", stdout);
            if (n->var_decl.ty->tag)
                fprintf(stdout, "%.*s ", n->var_decl.ty->tag->len,
                        n->var_decl.ty->tag->loc);
            fputs("{ ", stdout);
            for (int i = 0; i < n->var_decl.ty->enum_ntokens; i++) {
                Token *t = &n->var_decl.ty->enum_tokens[i];
                if (t->has_space && i > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
            fputs(" };\n", stdout);
            return;
        }
        /* Top-level free function declaration: 'int foo();' parses
         * as ND_VAR_DECL with TY_FUNC. emit_var_decl_inner doesn't
         * know how to print TY_FUNC (emit_type falls back to int),
         * so we synthesize the C declaration shape directly here. */
        if (n->var_decl.ty && n->var_decl.ty->kind == TY_FUNC &&
            n->var_decl.name) {
            if (func_dedup_check(n->var_decl.name)) return;
            emit_source_comment(n->tok);
            Type *fty = n->var_decl.ty;
            emit_type(fty->ret);
            fprintf(stdout, " %.*s(",
                    n->var_decl.name->len, n->var_decl.name->loc);
            if (fty->nparams == 0) {
                fputs("void", stdout);
            } else {
                for (int i = 0; i < fty->nparams; i++) {
                    if (i > 0) fputs(", ", stdout);
                    emit_type(fty->params[i]);
                }
                if (fty->is_variadic) fputs(", ...", stdout);
            }
            fputs(");\n", stdout);
            return;
        }
        emit_var_decl_inner(n);
        fputs(";\n", stdout);
        return;
    case ND_BLOCK:
        /* The parser packs namespace contents (and extern "C" blocks)
         * into ND_BLOCK at top level. Recurse into the inner decls. */
        for (int i = 0; i < n->block.nstmts; i++)
            emit_top_level(n->block.stmts[i]);
        return;
    case ND_TEMPLATE_DECL:
        /* Templates aren't lowered yet — silently skip. */
        return;
    case ND_TYPEDEF: {
        /* Emit the underlying type definition if applicable. */
        Type *uty = n->var_decl.ty;
        while (uty && (uty->kind == TY_PTR || uty->kind == TY_REF ||
                        uty->kind == TY_RVALREF))
            uty = uty->base;
        /* Struct/union with body */
        if (uty && (uty->kind == TY_STRUCT || uty->kind == TY_UNION) &&
            uty->class_def)
            emit_class_def(uty->class_def);
        /* Enum with body */
        if (uty && uty->kind == TY_ENUM &&
            uty->enum_tokens && uty->enum_ntokens > 0 &&
            !uty->codegen_emitted) {
            uty->codegen_emitted = true;
            fputs("enum ", stdout);
            if (uty->tag)
                fprintf(stdout, "%.*s ", uty->tag->len, uty->tag->loc);
            fputs("{ ", stdout);
            for (int i = 0; i < uty->enum_ntokens; i++) {
                Token *t = &uty->enum_tokens[i];
                if (t->has_space && i > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
            fputs(" };\n", stdout);
        }
        return;
    }
    default:
        fputs("/* unsupported top-level */\n", stdout);
        return;
    }
}

/* Sea-front cleanup-protocol prelude. Emitted at the top of every
 * translation unit. Identifiers are __SF_-prefixed: ISO C reserves
 * leading-double-underscore for the implementation, which we are.
 *
 * The unused-label pragma is needed because per-var cleanup labels
 * (one per dtor-bearing local) are not always referenced — only the
 * labels that some return/break/continue actually targets are reached
 * via goto; the others are walked only by fall-through. Suppressing
 * the warning is much cheaper than tracking per-label reference
 * counts at codegen time. */
static void emit_prelude(void) {
    fputs("/* generated by sea-front --emit-c */\n", stdout);
    fputs("#include <stdint.h>\n", stdout);
    fputs("#include <stddef.h>\n", stdout);  /* wchar_t, size_t, NULL */
    fputs("\n", stdout);
    fputs("/* sea-front cleanup protocol — see emit_c.c */\n", stdout);
    fputs("#if defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("#  pragma GCC diagnostic ignored \"-Wunused-label\"\n", stdout);
    fputs("#endif\n", stdout);
    fputs("typedef enum {\n", stdout);
    fputs("    __SF_UNWIND_NONE   = 0,\n", stdout);
    fputs("    __SF_UNWIND_RETURN = 1,\n", stdout);
    fputs("    __SF_UNWIND_BREAK  = 2,\n", stdout);
    fputs("    __SF_UNWIND_CONT   = 3,\n", stdout);
    fputs("} __SF_unwind_t;\n", stdout);
    /* Rewrite macros — set the unwind state and jump to the
     * innermost cleanup label (which then chains outward). */
    fputs("#define __SF_RETURN(v, lbl) "
          "do { __SF_retval = (v); __SF_unwind = __SF_UNWIND_RETURN; "
          "goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_RETURN_VOID(lbl) "
          "do { __SF_unwind = __SF_UNWIND_RETURN; goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_BREAK(lbl) "
          "do { __SF_unwind = __SF_UNWIND_BREAK; goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CONT(lbl) "
          "do { __SF_unwind = __SF_UNWIND_CONT; goto lbl; } while (0)\n",
          stdout);
    /* Function prologue/epilogue. PROLOGUE declares __SF_retval and
     * __SF_unwind for functions whose body has any non-trivial local;
     * the void variant skips __SF_retval. EPILOGUE emits the
     * __SF_epilogue: label and the actual C return. No do-while
     * wrapping — these emit declarations/labels, which can't sit
     * inside a statement expression. */
    /* '= {0}' is the universal C initializer — works for scalars
     * AND structs/unions/arrays. The bare '= 0' we used initially
     * blew up for struct return types. */
    fputs("#define __SF_PROLOGUE(ret_type) "
          "ret_type __SF_retval = {0}; "
          "__SF_unwind_t __SF_unwind = __SF_UNWIND_NONE; "
          "(void)__SF_unwind\n",
          stdout);
    fputs("#define __SF_PROLOGUE_VOID "
          "__SF_unwind_t __SF_unwind = __SF_UNWIND_NONE; "
          "(void)__SF_unwind\n",
          stdout);
    fputs("#define __SF_EPILOGUE __SF_epilogue: ; return __SF_retval\n", stdout);
    fputs("#define __SF_EPILOGUE_VOID __SF_epilogue: ; return\n", stdout);
    /* Chain macros — used at the bottom of each block-with-cleanups
     * after the dtor calls. ANY collapses to a single check when all
     * three unwind kinds target the same place; otherwise the three
     * per-kind macros emit a list of intentions, one per line. */
    fputs("#define __SF_CHAIN_ANY(lbl) "
          "do { if (__SF_unwind) goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_RETURN(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_RETURN) goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_BREAK(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_BREAK)  goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_CHAIN_CONT(lbl) "
          "do { if (__SF_unwind == __SF_UNWIND_CONT)   goto lbl; } while (0)\n",
          stdout);
    /* __SF_INLINE — multi-TU dedup for inline-eligible functions
     * (in-class methods, synthesized ctor/dtor wrappers, dtor body
     * functions, eventually template instantiations). Expands to a
     * weak-symbol attribute so the linker picks one survivor when
     * the same function is emitted in multiple TUs. See
     * docs/inline_and_dedup.md for the design rationale and full
     * trade-off analysis. */
    fputs("#if defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("#  define __SF_INLINE __attribute__((weak))\n", stdout);
    fputs("#elif defined(_MSC_VER)\n", stdout);
    fputs("#  define __SF_INLINE __declspec(selectany)\n", stdout);
    fputs("#else\n", stdout);
    fputs("#  define __SF_INLINE   /* fall back: each TU has its own copy */\n",
          stdout);
    fputs("#endif\n", stdout);
    fputs("\n", stdout);
}

/* Recursively emit forward declarations for all struct/union types
 * in the TU so ordering between classes (and template instantiations)
 * doesn't matter. */
static void emit_fwd_decl_walk(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_CLASS_DEF: {
        Type *ty = n->class_def.ty;
        if (ty && ty->tag) {
            fputs(ty->kind == TY_UNION ? "union " : "struct ", stdout);
            emit_mangled_class_tag(ty);
            fputs(";\n", stdout);
        }
        break;
    }
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            emit_fwd_decl_walk(n->block.stmts[i]);
        break;
    case ND_TEMPLATE_DECL:
        /* Don't forward-declare template bodies — only instantiated
         * copies (which appear as top-level ND_CLASS_DEF). */
        break;
    default:
        break;
    }
}

static void emit_forward_decl_structs(Node *tu) {
    for (int i = 0; i < tu->tu.ndecls; i++)
        emit_fwd_decl_walk(tu->tu.decls[i]);
}

void emit_c(Node *tu) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    emit_prelude();

    /* Forward-declare ALL struct types so pointer references
     * resolve regardless of definition order. */
    emit_forward_decl_structs(tu);

    /* Emit all enum definitions before any struct bodies, so enum
     * members used as struct fields have complete types. Enums
     * appear as ND_VAR_DECL with TY_ENUM (bare enum definition)
     * or as members of typedef/class nodes. */
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        /* Walk into blocks (namespace/extern "C" contents) */
        if (n->kind == ND_BLOCK) {
            for (int j = 0; j < n->block.nstmts; j++) {
                Node *s = n->block.stmts[j];
                if (!s) continue;
                Type *ety = NULL;
                if (s->kind == ND_VAR_DECL && s->var_decl.ty &&
                    s->var_decl.ty->kind == TY_ENUM)
                    ety = s->var_decl.ty;
                if (s->kind == ND_TYPEDEF && s->var_decl.ty &&
                    s->var_decl.ty->kind == TY_ENUM)
                    ety = s->var_decl.ty;
                if (ety && ety->enum_tokens && !ety->codegen_emitted) {
                    ety->codegen_emitted = true;
                    fputs("enum ", stdout);
                    if (ety->tag)
                        fprintf(stdout, "%.*s ", ety->tag->len, ety->tag->loc);
                    fputs("{ ", stdout);
                    for (int k = 0; k < ety->enum_ntokens; k++) {
                        Token *t = &ety->enum_tokens[k];
                        if (t->has_space && k > 0) fputc(' ', stdout);
                        fprintf(stdout, "%.*s", t->len, t->loc);
                    }
                    fputs(" };\n", stdout);
                }
            }
            continue;
        }
        /* Top-level enum */
        Type *ety = NULL;
        if (n->kind == ND_VAR_DECL && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_ENUM)
            ety = n->var_decl.ty;
        if (n->kind == ND_TYPEDEF && n->var_decl.ty &&
            n->var_decl.ty->kind == TY_ENUM)
            ety = n->var_decl.ty;
        if (ety && ety->enum_tokens && !ety->codegen_emitted) {
            ety->codegen_emitted = true;
            fputs("enum ", stdout);
            if (ety->tag)
                fprintf(stdout, "%.*s ", ety->tag->len, ety->tag->loc);
            fputs("{ ", stdout);
            for (int k = 0; k < ety->enum_ntokens; k++) {
                Token *t = &ety->enum_tokens[k];
                if (t->has_space && k > 0) fputc(' ', stdout);
                fprintf(stdout, "%.*s", t->len, t->loc);
            }
            fputs(" };\n", stdout);
        }
    }

    /* Two-pass emit: first all struct definitions (with forward
     * declarations for their methods), then all method bodies,
     * vtables, synthesized ctors/dtors, and non-class top-level
     * declarations. This ensures that ALL struct types are fully
     * defined before any method body dereferences a pointer to
     * another struct (e.g. vl_ptr methods accessing vl_embed
     * members through a pointer). */

    /* Pass 1: struct bodies only */
    g_emit_phase = PHASE_STRUCTS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        Node *n = tu->tu.decls[i];
        if (!n) continue;
        if (n->kind == ND_CLASS_DEF || n->kind == ND_TYPEDEF) {
            fputc('\n', stdout);
            emit_top_level(n);
        } else if (n->kind == ND_BLOCK) {
            /* Namespace blocks may contain class definitions */
            fputc('\n', stdout);
            emit_top_level(n);
        }
    }

    /* Pass 2: method bodies + everything else */
    g_emit_phase = PHASE_METHODS;
    for (int i = 0; i < tu->tu.ndecls; i++) {
        fputc('\n', stdout);
        emit_top_level(tu->tu.decls[i]);
    }
    g_emit_phase = 0;
}
