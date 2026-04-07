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
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
        return;
    case TY_UNION:
        fputs("union ", stdout);
        if (ty->tag) fprintf(stdout, "%.*s", ty->tag->len, ty->tag->loc);
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
         * gives us the class name to mangle with. */
        Node *callee = n->call.callee;
        if (callee && callee->kind == ND_MEMBER) {
            Node *obj = callee->member.obj;
            Type *ot = obj ? obj->resolved_type : NULL;
            bool obj_is_ptr = ot && ot->kind == TY_PTR;
            if (obj_is_ptr) ot = ot->base;
            if (ot && (ot->kind == TY_STRUCT || ot->kind == TY_UNION) &&
                ot->tag && callee->member.member) {
                fprintf(stdout, "%.*s_%.*s(",
                        ot->tag->len, ot->tag->loc,
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
    for (int i = 0; i < n->block.nstmts; i++) {
        emit_indent();
        emit_stmt(n->block.stmts[i]);
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
        fputs("return", stdout);
        if (n->ret.expr) {
            fputc(' ', stdout);
            emit_expr(n->ret.expr);
        }
        fputs(";\n", stdout);
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

static void emit_func_def(Node *n) {
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
    if (n->func.body)
        emit_block(n->func.body);
    else
        fputs(";\n", stdout);
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
static void emit_method_as_free_fn(Node *func, Token *class_tag) {
    if (!func || func->kind != ND_FUNC_DEF) return;
    if (!class_tag || !func->func.name) return;

    emit_type(func->func.ret_ty);
    fputc(' ', stdout);
    /* Mangled name: Class_method (no namespace mangling yet). */
    fprintf(stdout, "%.*s_%.*s",
            class_tag->len, class_tag->loc,
            func->func.name->len, func->func.name->loc);
    /* Parameter list: 'this' first, then the declared params. */
    fputc('(', stdout);
    fprintf(stdout, "struct %.*s *this", class_tag->len, class_tag->loc);
    for (int i = 0; i < func->func.nparams; i++) {
        fputs(", ", stdout);
        Node *p = func->func.params[i];
        emit_type(p->param.ty);
        if (p->param.name)
            fprintf(stdout, " %.*s",
                    p->param.name->len, p->param.name->loc);
    }
    fputs(") ", stdout);
    if (func->func.body)
        emit_block(func->func.body);
    else
        fputs(";\n", stdout);
}

static void emit_class_def(Node *n) {
    /* Emit a C struct from the parsed class definition.
     * Members handled: data members (ND_VAR_DECL with no init).
     * Skipped INSIDE the struct: methods (lowered to free functions
     * after the struct definition).
     * Other members (nested types, access specifiers) ignored. */
    fputs("struct ", stdout);
    if (n->class_def.tag)
        fprintf(stdout, "%.*s ",
                n->class_def.tag->len, n->class_def.tag->loc);
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

    /* Now emit each method (ND_FUNC_DEF in the member list) as a
     * separate free function. */
    for (int i = 0; i < n->class_def.nmembers; i++) {
        Node *m = n->class_def.members[i];
        if (m && m->kind == ND_FUNC_DEF)
            emit_method_as_free_fn(m, n->class_def.tag);
    }
}

static void emit_top_level(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case ND_FUNC_DEF: emit_func_def(n); return;
    case ND_CLASS_DEF: emit_class_def(n); return;
    case ND_VAR_DECL:
        emit_var_decl_inner(n);
        fputs(";\n", stdout);
        return;
    default:
        fputs("/* unsupported top-level */\n", stdout);
        return;
    }
}

void emit_c(Node *tu) {
    if (!tu || tu->kind != ND_TRANSLATION_UNIT) return;
    fputs("/* generated by sea-front --emit-c */\n", stdout);
    fputs("#include <stdint.h>\n", stdout);
    fputs("\n", stdout);
    for (int i = 0; i < tu->tu.ndecls; i++)
        emit_top_level(tu->tu.decls[i]);
}
