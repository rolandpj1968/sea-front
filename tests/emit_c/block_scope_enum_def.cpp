// EXPECT: 7
// A bare enum definition inside a function body parses as ND_VAR_DECL
// with TY_ENUM and no variable name. emit_top_level has a branch for
// this shape at file scope, but the block-scope path via emit_stmt's
// ND_VAR_DECL case went through emit_var_decl_inner, which printed
// only 'enum bb_state' (type without body). The enumerators were
// then undeclared and any use hit 'identifier undeclared'.
//
// Pattern: gcc 4.8 cfgrtl.c print_rtl_with_bb defines local
//   enum bb_state { NOT_IN_BB, IN_ONE_BB, IN_MULTIPLE_BB };
// and uses NOT_IN_BB / IN_ONE_BB as values. N4659 §10.2 [dcl.enum].

int main() {
    enum color { RED = 3, GREEN, BLUE };
    enum color c = GREEN;
    return c + RED;
}
