// EXPECT: 42
// Brace-init for non-class types ('uniform initialization'). The
// parser routes 'int x{7}' through the same ctor_args path as
// classes, but since the type isn't TY_STRUCT, codegen treats it
// as a copy-init in emit_var_decl_inner: 'int x = 7;'.
//
// Both forms 'int a{7}' and 'int b = {35}' are handled.
int main() {
    int a{7};
    int b = {35};
    return a + b;
}
