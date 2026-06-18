// EXPECT: 0
// Reference to function: `int (&foo)() = f;` — N4659 §11.3.2
// [dcl.ref]. Sea-front lowers refs to C function pointers but
// emit_var_decl_inner only handled the TY_PTR(TY_FUNC) shape, so
// TY_REF/TY_RVALREF base TY_FUNC fell through to a generic
// `emit_type(ty); name` form that produced `int (*)(void) foo`
// — invalid C (name must live INSIDE the grouped declarator).
//
// Add the parallel branch that emits
//   `int (*foo)(void) = &(f);`
// with `&` on the init so the rvalue function name yields the
// function pointer C expects in the slot.
//
// Reduced from g++.dg/other/default4.C `int(&foo1)() = f;`.

extern "C" void abort();

int counter = 0;
int f() { return ++counter; }

int (&foo)() = f;

int main() {
    if (foo() != 1) abort();    /* dispatch through the ref */
    if (foo() != 2) abort();
    if (counter != 2) abort();
    return 0;
}
