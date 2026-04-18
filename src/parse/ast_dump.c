/*
 * ast_dump.c — AST pretty-printer.
 *
 * Prints an S-expression style representation of the AST for
 * debugging and integration testing (--dump-ast mode). Output is
 * designed to be human-readable and diff-able. The format is
 * consumed verbatim by tests/parse/<name>.expected golden files, so
 * any change here is observable downstream.
 *
 * Example output (newlines and indentation roughly as emitted):
 *   (translation-unit
 *     (func-def int "main" (params)
 *       (block
 *         (return (num 0)))))
 *
 * Naming convention: node kinds use hyphens (func-def, var-decl,
 * class-def, ...), built-in types use hyphens for compound forms
 * (unsigned-int, long-long, unsigned-long-long), and operators
 * use their token-kind name from token_kind_name().
 */

#include "parse.h"

static void indent(int depth) {
    for (int i = 0; i < depth; i++)
        printf("  ");
}

static void dump_type(Type *ty) {
    if (!ty) { printf("?"); return; }

    if (ty->is_const) printf("const ");
    if (ty->is_volatile) printf("volatile ");

    switch (ty->kind) {
    case TY_VOID:    printf("void"); break;
    case TY_BOOL:    printf("bool"); break;
    case TY_CHAR:    printf(ty->is_unsigned ? "unsigned-char" : "char"); return;
    case TY_CHAR16:  printf("char16_t"); break;
    case TY_CHAR32:  printf("char32_t"); break;
    case TY_WCHAR:   printf("wchar_t"); break;
    case TY_SHORT:   printf(ty->is_unsigned ? "unsigned-short" : "short"); return;
    case TY_INT:     printf(ty->is_unsigned ? "unsigned-int" : "int"); return;
    case TY_LONG:    printf(ty->is_unsigned ? "unsigned-long" : "long"); return;
    case TY_LLONG:   printf(ty->is_unsigned ? "unsigned-long-long" : "long-long"); return;
    case TY_FLOAT:   printf("float"); break;
    case TY_DOUBLE:  printf("double"); break;
    case TY_LDOUBLE: printf("long-double"); break;
    case TY_PTR:
        printf("(ptr ");
        dump_type(ty->base);
        printf(")");
        return;
    case TY_REF:
        printf("(ref ");
        dump_type(ty->base);
        printf(")");
        return;
    case TY_RVALREF:
        printf("(rvalref ");
        dump_type(ty->base);
        printf(")");
        return;
    case TY_ARRAY:
        printf("(array ");
        dump_type(ty->base);
        if (ty->array_len >= 0)
            printf(" %d", ty->array_len);
        printf(")");
        return;
    case TY_FUNC:
        printf("(func-type ");
        dump_type(ty->ret);
        printf(" (");
        for (int i = 0; i < ty->nparams; i++) {
            if (i > 0) printf(" ");
            dump_type(ty->params[i]);
        }
        if (ty->is_variadic)
            printf(" ...");
        printf("))");
        return;
    case TY_STRUCT:
        printf("struct");
        if (ty->tag) printf(" %.*s", ty->tag->len, ty->tag->loc);
        if (ty->n_template_args > 0) {
            printf("<");
            for (int i = 0; i < ty->n_template_args; i++) {
                if (i > 0) printf(",");
                dump_type(ty->template_args[i]);
            }
            printf(">");
        }
        return;
    case TY_UNION:
        printf("union");
        if (ty->tag) printf(" %.*s", ty->tag->len, ty->tag->loc);
        return;
    case TY_ENUM:
        printf("enum");
        if (ty->tag) printf(" %.*s", ty->tag->len, ty->tag->loc);
        return;
    case TY_DEPENDENT:
        printf("dependent");
        if (ty->tag) printf(" %.*s", ty->tag->len, ty->tag->loc);
        return;
    }
}

static void dump(Node *node, int depth);

static void dump_children(Node **nodes, int count, int depth) {
    for (int i = 0; i < count; i++) {
        printf("\n");
        indent(depth);
        dump(nodes[i], depth);
    }
}

static void dump(Node *node, int depth) {
    if (!node) { printf("(null)"); return; }

    switch (node->kind) {
    case ND_NUM:
        if (node->num.hi)
            printf("(num 0x%lx%016lx)", (unsigned long)node->num.hi,
                   (unsigned long)node->num.lo);
        else
            printf("(num %lu)", (unsigned long)node->num.lo);
        break;

    case ND_FNUM:
        printf("(fnum %g)", node->fnum.fval);
        break;

    case ND_BOOL_LIT:
        printf("(%s)", node->tok->kind == TK_KW_TRUE ? "true" : "false");
        break;

    case ND_NULLPTR:
        printf("(nullptr)");
        break;

    case ND_STR:
        printf("(str '%.*s')", node->str.tok->len, node->str.tok->loc);
        break;

    case ND_CHAR:
        printf("(char '%.*s')", node->chr.tok->len, node->chr.tok->loc);
        break;

    case ND_IDENT:
        printf("(ident \"%.*s\")", node->ident.name->len, node->ident.name->loc);
        break;

    case ND_QUALIFIED:
        printf("(qualified");
        if (node->qualified.global_scope)
            printf(" ::");
        for (int i = 0; i < node->qualified.nparts; i++)
            printf(" \"%.*s\"", node->qualified.parts[i]->len,
                   node->qualified.parts[i]->loc);
        printf(")");
        break;

    case ND_BINARY:
    case ND_ASSIGN:
        printf("(%s ", token_kind_name(node->binary.op));
        dump(node->binary.lhs, depth + 1);
        printf(" ");
        dump(node->binary.rhs, depth + 1);
        printf(")");
        break;

    case ND_UNARY:
    case ND_POSTFIX:
        printf("(%s%s ",
               node->kind == ND_POSTFIX ? "post-" : "",
               token_kind_name(node->unary.op));
        dump(node->unary.operand, depth + 1);
        printf(")");
        break;

    case ND_TERNARY:
        printf("(?: ");
        dump(node->ternary.cond, depth + 1);
        printf(" ");
        dump(node->ternary.then_, depth + 1);
        printf(" ");
        dump(node->ternary.else_, depth + 1);
        printf(")");
        break;

    case ND_COMMA:
        printf("(, ");
        dump(node->comma.lhs, depth + 1);
        printf(" ");
        dump(node->comma.rhs, depth + 1);
        printf(")");
        break;

    case ND_CALL:
        printf("(call ");
        dump(node->call.callee, depth + 1);
        for (int i = 0; i < node->call.nargs; i++) {
            printf(" ");
            dump(node->call.args[i], depth + 1);
        }
        printf(")");
        break;

    case ND_MEMBER:
        printf("(%s ", token_kind_name(node->member.op));
        dump(node->member.obj, depth + 1);
        printf(" \"%.*s\")", node->member.member->len, node->member.member->loc);
        break;

    case ND_SUBSCRIPT:
        printf("([] ");
        dump(node->subscript.base, depth + 1);
        printf(" ");
        dump(node->subscript.index, depth + 1);
        printf(")");
        break;

    case ND_CAST:
        printf("(cast ");
        dump_type(node->cast.ty);
        printf(" ");
        dump(node->cast.operand, depth + 1);
        printf(")");
        break;

    case ND_SIZEOF:
        printf("(sizeof ");
        if (node->sizeof_.is_type)
            dump_type(node->sizeof_.ty);
        else
            dump(node->sizeof_.expr, depth + 1);
        printf(")");
        break;

    case ND_ALIGNOF:
        printf("(alignof ");
        dump_type(node->alignof_.ty);
        printf(")");
        break;

    case ND_BLOCK:
        printf("(block");
        dump_children(node->block.stmts, node->block.nstmts, depth + 1);
        printf(")");
        break;

    case ND_RETURN:
        printf("(return");
        if (node->ret.expr) {
            printf(" ");
            dump(node->ret.expr, depth + 1);
        }
        printf(")");
        break;

    case ND_EXPR_STMT:
        dump(node->expr_stmt.expr, depth);
        break;

    case ND_NULL_STMT:
        printf("(null-stmt)");
        break;

    case ND_IF:
        printf("(if ");
        if (node->if_.init) {
            printf("(init ");
            dump(node->if_.init, depth + 1);
            printf(") ");
        }
        dump(node->if_.cond, depth + 1);
        printf("\n");
        indent(depth + 1);
        dump(node->if_.then_, depth + 1);
        if (node->if_.else_) {
            printf("\n");
            indent(depth + 1);
            dump(node->if_.else_, depth + 1);
        }
        printf(")");
        break;

    case ND_WHILE:
        printf("(while ");
        dump(node->while_.cond, depth + 1);
        printf("\n");
        indent(depth + 1);
        dump(node->while_.body, depth + 1);
        printf(")");
        break;

    case ND_DO:
        printf("(do\n");
        indent(depth + 1);
        dump(node->do_.body, depth + 1);
        printf("\n");
        indent(depth + 1);
        dump(node->do_.cond, depth + 1);
        printf(")");
        break;

    case ND_FOR:
        printf("(for ");
        if (node->for_.init) dump(node->for_.init, depth + 1);
        else printf("(null)");
        printf(" ");
        if (node->for_.cond) dump(node->for_.cond, depth + 1);
        else printf("(null)");
        printf(" ");
        if (node->for_.inc) dump(node->for_.inc, depth + 1);
        else printf("(null)");
        printf("\n");
        indent(depth + 1);
        dump(node->for_.body, depth + 1);
        printf(")");
        break;

    case ND_SWITCH:
        printf("(switch ");
        dump(node->switch_.expr, depth + 1);
        printf("\n");
        indent(depth + 1);
        dump(node->switch_.body, depth + 1);
        printf(")");
        break;

    case ND_CASE:
        printf("(case ");
        dump(node->case_.expr, depth + 1);
        printf("\n");
        indent(depth + 1);
        dump(node->case_.stmt, depth + 1);
        printf(")");
        break;

    case ND_DEFAULT:
        printf("(default\n");
        indent(depth + 1);
        dump(node->default_.stmt, depth + 1);
        printf(")");
        break;

    case ND_BREAK:    printf("(break)"); break;
    case ND_CONTINUE: printf("(continue)"); break;

    case ND_GOTO:
        printf("(goto \"%.*s\")", node->goto_.label->len, node->goto_.label->loc);
        break;

    case ND_LABEL:
        printf("(label \"%.*s\"\n", node->label.label->len, node->label.label->loc);
        indent(depth + 1);
        dump(node->label.stmt, depth + 1);
        printf(")");
        break;

    case ND_VAR_DECL:
        printf("(var-decl ");
        dump_type(node->var_decl.ty);
        if (node->var_decl.name)
            printf(" \"%.*s\"", node->var_decl.name->len, node->var_decl.name->loc);
        if (node->var_decl.init) {
            printf(" ");
            dump(node->var_decl.init, depth + 1);
        }
        if (node->var_decl.has_ctor_init) {
            printf(" (ctor-args");
            for (int i = 0; i < node->var_decl.ctor_nargs; i++) {
                printf(" ");
                dump(node->var_decl.ctor_args[i], depth + 1);
            }
            printf(")");
        }
        printf(")");
        break;

    case ND_FUNC_DEF:
    case ND_FUNC_DECL:
        printf("(%s ", node->kind == ND_FUNC_DEF ? "func-def" : "func-decl");
        /* Storage/qualifier flags — show only when set.
         * Use storage_flags uniformly (§10.1 [dcl.spec]). */
        {
            int sf = node->func.storage_flags;
            if (sf & DECL_STATIC)    printf("static ");
            if (sf & DECL_INLINE)    printf("inline ");
            if (sf & DECL_EXTERN)    printf("extern ");
            if (sf & DECL_VIRTUAL)   printf("virtual ");
            if (sf & DECL_CONSTEXPR) printf("constexpr ");
        }
        if (node->func.is_constructor)  printf("ctor ");
        if (node->func.is_destructor)   printf("dtor ");
        if (node->func.is_const_method) printf("const ");
        dump_type(node->func.ret_ty);
        if (node->func.name)
            printf(" \"%.*s\"", node->func.name->len, node->func.name->loc);
        printf(" (params");
        for (int i = 0; i < node->func.nparams; i++) {
            printf(" ");
            dump(node->func.params[i], depth + 1);
        }
        printf(")");
        if (node->func.body) {
            printf("\n");
            indent(depth + 1);
            dump(node->func.body, depth + 1);
        }
        printf(")");
        break;

    case ND_PARAM:
        printf("(param ");
        dump_type(node->param.ty);
        if (node->param.name)
            printf(" \"%.*s\"", node->param.name->len, node->param.name->loc);
        printf(")");
        break;

    case ND_TYPEDEF:
        printf("(typedef)");
        break;

    case ND_TEMPLATE_DECL:
        printf("(template (params");
        for (int i = 0; i < node->template_decl.nparams; i++) {
            printf(" ");
            dump(node->template_decl.params[i], depth + 1);
        }
        printf(")");
        if (node->template_decl.decl) {
            printf("\n");
            indent(depth + 1);
            dump(node->template_decl.decl, depth + 1);
        }
        printf(")");
        break;

    case ND_TEMPLATE_ID:
        printf("(template-id \"%.*s\"", node->template_id.name->len,
               node->template_id.name->loc);
        for (int i = 0; i < node->template_id.nargs; i++) {
            printf(" ");
            dump(node->template_id.args[i], depth + 1);
        }
        printf(")");
        break;

    case ND_CLASS_DEF:
        printf("(class-def");
        if (node->class_def.tag)
            printf(" \"%.*s\"", node->class_def.tag->len,
                   node->class_def.tag->loc);
        dump_children(node->class_def.members, node->class_def.nmembers,
                      depth + 1);
        printf(")");
        break;

    case ND_ACCESS_SPEC: {
        const char *acc = "unknown";
        if (node->access_spec.access == TK_KW_PUBLIC) acc = "public";
        else if (node->access_spec.access == TK_KW_PROTECTED) acc = "protected";
        else if (node->access_spec.access == TK_KW_PRIVATE) acc = "private";
        printf("(%s)", acc);
        break;
    }

    case ND_FRIEND:
        printf("(friend ");
        dump(node->friend_decl.decl, depth + 1);
        printf(")");
        break;

    case ND_TRANSLATION_UNIT:
        printf("(translation-unit");
        dump_children(node->tu.decls, node->tu.ndecls, depth + 1);
        printf(")");
        break;
    }
}

void dump_ast(Node *node, int depth) {
    dump(node, depth);
    printf("\n");
}
