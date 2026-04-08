/*
 * emit_c.c — AST → C codegen (first slice).
 *
 * Walks the AST and emits C. Currently handles:
 *   - Function definitions and parameter lists with built-in types
 *   - Variable declarations with optional init
 *   - Return, if/else, while, for, do-while, break/continue
 *   - Compound statements
 *   - Arithmetic, relational, logical, assignment binary expressions
 *   - Unary expressions, postfix ++/--, ternary
 *   - Integer / float / char / bool literals
 *   - Identifiers and member access (best-effort, source-form)
 *   - Function calls with positional args
 *
 * Out of scope (skipped or emitted as a comment):
 *   - Classes, member functions, templates
 *   - References (we emit '*' instead — fine for the first slice
 *     since we're targeting built-in types)
 *   - Operator overloads, casts beyond C-style
 *
 * The output is intentionally minimal — no formatting cleverness.
 */

#include <stdio.h>
#include "emit_c.h"
#include "../sea-front.h"

static int g_indent = 0;

static void emit_indent(void) {
    for (int i = 0; i < g_indent; i++) fputs("    ", stdout);
}

/* ------------------------------------------------------------------ */
/* Per-function codegen state for destructor lowering (Slice B)        */
/*                                                                     */
/* When the function body contains any local with a non-trivial dtor   */
/* we lower 'return expr' as:                                          */
/*     __retval = expr; __unwind = 1; goto __cleanup_<innermost>;      */
/* Each block carrying cleanups emits a label, runs its dtors, and     */
/* conditionally chains outward via 'if (__unwind) goto <parent>'.    */
/* The function epilogue runs 'return __retval;'.                      */
/*                                                                     */
/* This is per-function state, not nested blocks, so a flat module     */
/* global is fine — codegen is single-threaded and not reentrant.      */
/* ------------------------------------------------------------------ */

typedef struct CleanupFrame {
    int   label_id;     /* unique within the function; -1 if no cleanups */
    bool  has_dtors;    /* this block actually emits at least one dtor */
} CleanupFrame;

#define CLEANUP_STACK_MAX 32

static struct {
    bool          func_has_cleanups;  /* function-wide flag, set by pre-scan */
    int           next_label_id;      /* fresh-id counter for __cleanup_<n> */
    CleanupFrame  stack[CLEANUP_STACK_MAX];
    int           depth;              /* number of frames currently pushed */
} g_cf;

/* Find the innermost cleanup frame that has dtors, walking outward.
 * Returns its label_id, or -1 if none — in which case 'return' jumps
 * directly to __epilogue. */
static int innermost_cleanup_label(void) {
    for (int i = g_cf.depth - 1; i >= 0; i--) {
        if (g_cf.stack[i].has_dtors)
            return g_cf.stack[i].label_id;
    }
    return -1;
}

/* Pre-scan: does any block in this subtree declare a class instance
 * with a non-trivial dtor? If yes, the function needs the cleanup
 * machinery (locals __retval/__unwind, __epilogue label, return rewrite). */
static bool subtree_has_cleanups(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case ND_VAR_DECL:
        return n->var_decl.ty && n->var_decl.ty->kind == TY_STRUCT &&
               n->var_decl.ty->has_dtor;
    case ND_BLOCK:
        for (int i = 0; i < n->block.nstmts; i++)
            if (subtree_has_cleanups(n->block.stmts[i])) return true;
        return false;
    case ND_IF:
        return subtree_has_cleanups(n->if_.then_) ||
               subtree_has_cleanups(n->if_.else_);
    case ND_WHILE:
        return subtree_has_cleanups(n->while_.body);
    case ND_DO:
        return subtree_has_cleanups(n->do_.body);
    case ND_FOR:
        return subtree_has_cleanups(n->for_.init) ||
               subtree_has_cleanups(n->for_.body);
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Name mangling                                                       */
/* ------------------------------------------------------------------ */

/* Walk a class type's class_region enclosing chain to find namespaces
 * and emit them as a prefix. Result: 'ns1_ns2_'. Empty string for a
 * class at global scope. The trailing underscore is included so
 * callers can append the class tag directly. */
static void emit_ns_prefix(Type *class_type) {
    if (!class_type || !class_type->class_region) return;
    /* Walk OUT from the class to collect namespace names. */
    enum { MAX_NS = 8 };
    Token *names[MAX_NS];
    int n = 0;
    DeclarativeRegion *r = class_type->class_region->enclosing;
    while (r && n < MAX_NS) {
        if (r->kind == REGION_NAMESPACE && r->name)
            names[n++] = r->name;
        r = r->enclosing;
    }
    /* Emit outermost first. */
    for (int i = n - 1; i >= 0; i--)
        fprintf(stdout, "%.*s_", names[i]->len, names[i]->loc);
}

/* Emit the mangled struct/class tag for a class type:
 *   global   — 'Tag'
 *   ns::Tag  — 'ns_Tag'
 *   a::b::T  — 'a_b_T'
 * Used both for the C struct tag (so two classes named the same in
 * different namespaces don't collide in the single C tag namespace)
 * and as the prefix for method mangling. */
static void emit_mangled_class_tag(Type *class_type) {
    if (!class_type || !class_type->tag) {
        fputs("?", stdout);
        return;
    }
    emit_ns_prefix(class_type);
    fprintf(stdout, "%.*s",
            class_type->tag->len, class_type->tag->loc);
}

/* ------------------------------------------------------------------ */
/* Type emission                                                       */
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
    default:
        fputs("/*?*/ int", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Expression emission                                                 */
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
        /* C99: stdbool true/false. We'd need #include <stdbool.h>. Use 1/0
         * for portability — bool maps to _Bool. */
        emit_token_text(n->tok);
        return;
    case ND_NULLPTR:
        fputs("((void*)0)", stdout);
        return;
    case ND_STR:
        if (n->str.tok) fprintf(stdout, "%.*s", n->str.tok->len, n->str.tok->loc);
        return;
    case ND_IDENT:
        if (n->ident.implicit_this) fputs("this->", stdout);
        emit_token_text(n->ident.name);
        return;
    case ND_BINARY:
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        emit_expr(n->binary.rhs);
        fputc(')', stdout);
        return;
    case ND_ASSIGN:
        fputc('(', stdout);
        emit_expr(n->binary.lhs);
        fprintf(stdout, " %s ", binop_str(n->binary.op));
        emit_expr(n->binary.rhs);
        fputc(')', stdout);
        return;
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
            emit_mangled_class_tag(class_type);
            fprintf(stdout, "_%.*s(this",
                    mname->len, mname->loc);
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
                emit_mangled_class_tag(ot);
                fprintf(stdout, "_%.*s(",
                        callee->member.member->len, callee->member.member->loc);
                /* this argument: address-of for value, as-is for pointer. */
                if (obj_is_ptr) {
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
    case ND_MEMBER:
        emit_expr(n->member.obj);
        fputs(n->member.op == TK_ARROW ? "->" : ".", stdout);
        if (n->member.member)
            fprintf(stdout, "%.*s",
                    n->member.member->len, n->member.member->loc);
        return;
    case ND_SUBSCRIPT:
        emit_expr(n->subscript.base);
        fputc('[', stdout);
        emit_expr(n->subscript.index);
        fputc(']', stdout);
        return;
    default:
        fputs("/* expr */", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Statement emission                                                  */
/* ------------------------------------------------------------------ */

static void emit_stmt(Node *n);

static void emit_var_decl_inner(Node *n) {
    emit_type(n->var_decl.ty);
    fputc(' ', stdout);
    if (n->var_decl.name)
        fprintf(stdout, "%.*s", n->var_decl.name->len, n->var_decl.name->loc);
    if (n->var_decl.init) {
        fputs(" = ", stdout);
        emit_expr(n->var_decl.init);
    }
}

static void emit_block(Node *n) {
    fputs("{\n", stdout);
    g_indent++;

    /* Dtor lowering — Slice B (single-call-site goto chain).
     *
     * Pass 1: scan the block's statements for class locals with a
     * non-trivial dtor. If we find any, allocate a fresh cleanup
     * label id for this block; otherwise the block doesn't appear
     * in the chain and a 'return' inside it will jump straight to
     * the next ancestor (or __epilogue).
     *
     * Pass 2: emit user statements with the frame on the stack —
     * that way nested ND_RETURN can consult innermost_cleanup_label().
     *
     * Pass 3: at the closing brace, emit the cleanup label, run
     * dtors in reverse declaration order, and chain outward via
     * 'if (__unwind) goto <parent>' / __epilogue. */
    enum { CLEANUP_MAX = 64 };
    Node *cleanup_var[CLEANUP_MAX];
    int   ncleanup = 0;

    for (int i = 0; i < n->block.nstmts; i++) {
        Node *s = n->block.stmts[i];
        if (s && s->kind == ND_VAR_DECL && s->var_decl.ty &&
            s->var_decl.ty->kind == TY_STRUCT && s->var_decl.ty->has_dtor &&
            s->var_decl.name && ncleanup < CLEANUP_MAX) {
            cleanup_var[ncleanup++] = s;
        }
    }

    CleanupFrame frame;
    frame.has_dtors = (ncleanup > 0) && g_cf.func_has_cleanups;
    frame.label_id  = frame.has_dtors ? g_cf.next_label_id++ : -1;
    if (g_cf.depth < CLEANUP_STACK_MAX)
        g_cf.stack[g_cf.depth++] = frame;

    for (int i = 0; i < n->block.nstmts; i++) {
        emit_indent();
        emit_stmt(n->block.stmts[i]);
    }

    if (frame.has_dtors) {
        /* The label sits at the bottom of the block and is reached
         * by both normal fall-through and 'goto' from a nested
         * return-with-cleanup. ';' after the label keeps it a valid
         * statement when the next thing is a declaration. */
        emit_indent();
        fprintf(stdout, "__SF_cleanup_%d: ;\n", frame.label_id);
        for (int i = ncleanup - 1; i >= 0; i--) {
            Node *v = cleanup_var[i];
            emit_indent();
            emit_mangled_class_tag(v->var_decl.ty);
            fprintf(stdout, "_dtor(&%.*s);\n",
                    v->var_decl.name->len, v->var_decl.name->loc);
        }
        /* Chain outward when unwinding. We have to look at the
         * stack BELOW our own frame, since 'depth' still includes
         * us at this point. */
        g_cf.depth--;
        int parent = innermost_cleanup_label();
        emit_indent();
        if (parent >= 0)
            fprintf(stdout, "if (__SF_unwind) goto __SF_cleanup_%d;\n", parent);
        else
            fputs("if (__SF_unwind) goto __SF_epilogue;\n", stdout);
    } else {
        if (g_cf.depth > 0) g_cf.depth--;
    }

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
            int target = innermost_cleanup_label();
            const char *target_str_buf = NULL;
            char buf[32];
            if (target >= 0) {
                snprintf(buf, sizeof(buf), "__SF_cleanup_%d", target);
                target_str_buf = buf;
            } else {
                target_str_buf = "__SF_epilogue";
            }
            if (n->ret.expr) {
                fputs("__SF_RETURN(", stdout);
                emit_expr(n->ret.expr);
                fprintf(stdout, ", %s);\n", target_str_buf);
            } else {
                fprintf(stdout, "__SF_RETURN_VOID(%s);\n", target_str_buf);
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
        return;
    case ND_IF:
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
    case ND_WHILE:
        fputs("while (", stdout);
        emit_expr(n->while_.cond);
        fputs(") ", stdout);
        emit_stmt(n->while_.body);
        return;
    case ND_FOR:
        fputs("for (", stdout);
        if (n->for_.init) {
            /* Inline form: emit a var-decl or expression without the
             * trailing ';\n' that emit_stmt would produce. */
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
        emit_stmt(n->for_.body);
        return;
    case ND_BREAK:
        fputs("break;\n", stdout);
        return;
    case ND_CONTINUE:
        fputs("continue;\n", stdout);
        return;
    default:
        fputs("/* stmt */;\n", stdout);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Top-level emission                                                  */
/* ------------------------------------------------------------------ */

/* Reset the per-function dtor lowering state and decide whether the
 * function needs the cleanup machinery at all. Called at the entry
 * to every function/method emission. */
static void cf_begin_function(Node *func) {
    g_cf.next_label_id = 0;
    g_cf.depth = 0;
    g_cf.func_has_cleanups = func && func->func.body &&
                             subtree_has_cleanups(func->func.body);
}

/* Emit the body of a function with cleanup-aware wrapping when the
 * function has any non-trivial locals: declare __retval/__unwind at
 * the top, run the body, then __epilogue: return __retval; */
static void emit_func_body(Node *func) {
    if (!func->func.body) { fputs(";\n", stdout); return; }
    if (!g_cf.func_has_cleanups) {
        emit_block(func->func.body);
        return;
    }
    /* Wrap: open a brace, declare locals, emit the user body
     * (which is itself a ND_BLOCK and will print its own { }),
     * then the epilogue. */
    fputs("{\n", stdout);
    g_indent++;
    emit_indent();
    /* __SF_retval is typed as the function's return type. For void
     * returns we skip __SF_retval entirely (no value to forward). */
    bool void_ret = func->func.ret_ty && func->func.ret_ty->kind == TY_VOID;
    if (!void_ret) {
        emit_type(func->func.ret_ty);
        fputs(" __SF_retval = 0;\n", stdout);
        emit_indent();
    }
    fputs("int __SF_unwind = 0;\n", stdout);
    emit_indent();
    fputs("(void)__SF_unwind;\n", stdout);  /* silence unused if no return */
    emit_indent();
    emit_block(func->func.body);
    emit_indent();
    fputs("__SF_epilogue: ;\n", stdout);
    emit_indent();
    if (void_ret)
        fputs("return;\n", stdout);
    else
        fputs("return __SF_retval;\n", stdout);
    g_indent--;
    emit_indent();
    fputs("}\n", stdout);
}

static void emit_func_def(Node *n) {
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
static void emit_method_signature(Node *func, Type *class_type) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_type || !class_type->tag || !func->func.name) return;

    emit_type(func->func.ret_ty);
    fputc(' ', stdout);
    emit_mangled_class_tag(class_type);
    if (func->func.is_destructor) {
        /* Mangle dtors as Class_dtor so they don't collide with a
         * same-named ctor (Class::Class). The name token still points
         * at the class identifier — we ignore it for dtors. */
        fputs("_dtor", stdout);
    } else {
        fprintf(stdout, "_%.*s",
                func->func.name->len, func->func.name->loc);
    }
    fputc('(', stdout);
    fputs("struct ", stdout);
    emit_mangled_class_tag(class_type);
    fputs(" *this", stdout);
    for (int i = 0; i < func->func.nparams; i++) {
        fputs(", ", stdout);
        Node *p = func->func.params[i];
        emit_type(p->param.ty);
        if (p->param.name)
            fprintf(stdout, " %.*s",
                    p->param.name->len, p->param.name->loc);
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

    cf_begin_function(func);
    emit_method_signature(func, class_type);
    fputc(' ', stdout);
    emit_func_body(func);
}

static void emit_class_def(Node *n) {
    /* Emit a C struct from the parsed class definition.
     * Members handled: data members (ND_VAR_DECL with no init).
     * Skipped INSIDE the struct: methods (lowered to free functions
     * after the struct definition).
     * Other members (nested types, access specifiers) ignored. */
    Type *class_type = n->class_def.ty;
    fputs("struct ", stdout);
    if (class_type)
        emit_mangled_class_tag(class_type);
    else if (n->class_def.tag)
        fprintf(stdout, "%.*s",
                n->class_def.tag->len, n->class_def.tag->loc);
    fputc(' ', stdout);
    fputs("{\n", stdout);
    g_indent++;
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        if (m->kind != ND_VAR_DECL) continue;
        /* Skip member functions: ND_VAR_DECL with TY_FUNC type. */
        if (m->var_decl.ty && m->var_decl.ty->kind == TY_FUNC) continue;
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
     * count. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (!m) continue;
        if (m->kind == ND_FUNC_DEF && class_type) {
            emit_method_signature(m, class_type);
            fputs(";\n", stdout);
        } else if (m->kind == ND_VAR_DECL && m->var_decl.ty &&
                   m->var_decl.ty->kind == TY_FUNC && m->var_decl.name &&
                   class_type) {
            /* Synthesise an emit_method_signature-like header from the
             * var-decl's type. */
            Type *fty = m->var_decl.ty;
            emit_type(fty->ret);
            fputc(' ', stdout);
            emit_mangled_class_tag(class_type);
            fprintf(stdout, "_%.*s(struct ",
                    m->var_decl.name->len, m->var_decl.name->loc);
            emit_mangled_class_tag(class_type);
            fputs(" *this", stdout);
            for (int k = 0; k < fty->nparams; k++) {
                fputs(", ", stdout);
                emit_type(fty->params[k]);
            }
            fputs(");\n", stdout);
        }
    }

    /* Now emit each method (ND_FUNC_DEF in the member list) as a
     * separate free function. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (m && m->kind == ND_FUNC_DEF && class_type)
            emit_method_as_free_fn(m, class_type);
    }
}

static void emit_top_level(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC_DEF:
        /* Out-of-class method definition 'int Foo::bar() {}' was
         * tagged by the parser with the resolved class type. Emit
         * it as a mangled free function with the 'this' parameter
         * prepended. */
        if (n->func.class_type && n->func.class_type->tag)
            emit_method_as_free_fn(n, n->func.class_type);
        else
            emit_func_def(n);
        return;
    case ND_CLASS_DEF: emit_class_def(n); return;
    case ND_VAR_DECL:
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
    fputs("\n", stdout);
    fputs("/* sea-front cleanup protocol — see emit_c.c */\n", stdout);
    fputs("#if defined(__GNUC__) || defined(__clang__)\n", stdout);
    fputs("#  pragma GCC diagnostic ignored \"-Wunused-label\"\n", stdout);
    fputs("#endif\n", stdout);
    fputs("#define __SF_RETURN(v, lbl) "
          "do { __SF_retval = (v); __SF_unwind = 1; goto lbl; } while (0)\n",
          stdout);
    fputs("#define __SF_RETURN_VOID(lbl) "
          "do { __SF_unwind = 1; goto lbl; } while (0)\n",
          stdout);
    fputs("\n", stdout);
}

void emit_c(Node *tu) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    emit_prelude();
    for (int i = 0; i < tu->tu.ndecls; i++)
        emit_top_level(tu->tu.decls[i]);
}
